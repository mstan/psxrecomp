#include <cstdio>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <algorithm>
#include <set>
#include <vector>

#include "ps1_exe_parser.h"
#include "gte.h"
#include "function_analysis.h"
#include "control_flow.h"
#include "code_generator.h"
#include "annotations.hpp"
#include "config_loader.h"
#include "rabbitizer.hpp"
#include "fmt/format.h"
#include "write_if_changed.h"

/* Baked emitter-source hash (recompiler/CMakeLists.txt custom command; same
 * hash_codegen.cmake + canonical source list as the runtime's overlay cache
 * tag). Exposed via --codegen-hash so compile_overlays.py can verify THIS
 * binary was built from the same emitter sources as the cg tag it stamps —
 * the stale-recompiler-binary guard. 0 when built without the bake (guard
 * consumers treat that as "cannot verify" and fail loudly). */
#if defined(__has_include)
#  if __has_include("psxrecomp_baked_codegen_hash.h")
#    include "psxrecomp_baked_codegen_hash.h"
#  endif
#endif
#ifndef PSX_OVERLAY_CODEGEN_HASH
#  define PSX_OVERLAY_CODEGEN_HASH 0u
#endif

namespace {

struct AliasEntry { uint32_t addr, host_start, host_end; };

// Materialize alias entries as overlapping functions, grouped by host. Each
// group shares one emitted body (entry switch + host-range blocks); every
// member lists its siblings so all group CFGs carry identical block leaders.
// Host functions are emitted unchanged — aliases never cap or truncate them.
void materialize_alias_groups(PSXRecomp::FunctionAnalysisResult& result,
                              const std::vector<AliasEntry>& alias_entries) {
    std::map<uint32_t, std::vector<const AliasEntry*>> by_host;
    for (const auto& ae : alias_entries) {
        by_host[ae.host_start].push_back(&ae);
    }
    for (const auto& [host_start, group] : by_host) {
        uint32_t producer_lo = 0;
        uint32_t producer_hi = 0;
        for (const auto& function : result.functions) {
            if (function.start_addr == host_start) {
                producer_lo = function.producer_lo;
                producer_hi = function.producer_hi;
                break;
            }
        }
        std::vector<uint32_t> entries;
        for (const AliasEntry* ae : group) entries.push_back(ae->addr);
        for (const AliasEntry* ae : group) {
            PSXRecomp::Function af;
            af.start_addr = ae->addr;
            af.end_addr = ae->host_end;
            af.size = af.end_addr - af.start_addr;
            af.has_prologue = false;
            af.has_epilogue = false;
            af.stack_frame_size = 0;
            af.name = fmt::format("func_{:08X}", ae->addr);
            af.is_data_section = false;
            af.alias_walk_lo = ae->host_start;
            af.alias_group_entries = entries;
            af.producer_lo = producer_lo;
            af.producer_hi = producer_hi;
            result.functions.push_back(af);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto print_usage = [&]() {
        fmt::print("Usage: {} --config <game.toml>\n", argv[0]);
        fmt::print("       {} <PS1-EXE file> [--seeds <file>] [--out-dir <dir>] [--strict] [--inspect]\n", argv[0]);
        fmt::print("Example: {} SCUS_942.36 --seeds seeds/functions.txt --out-dir generated --strict\n", argv[0]);
        fmt::print("\n");
        fmt::print("  --project-root <dir>  Resolve the BIOS profile (bios/SCPH1001.toml, or\n");
        fmt::print("                        <framework>/bios/SCPH1001.toml one level down) against\n");
        fmt::print("                        <dir> instead of the CWD. Required whenever the caller\n");
        fmt::print("                        cannot choose its own CWD — e.g. compile_overlays.py,\n");
        fmt::print("                        which runs us with cwd = dirname(game.toml).\n");
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
    }
    if (argc < 2) {
        print_usage();
        return 0;
    }

    /* --codegen-hash: print the baked emitter-source hash and exit. Handled
     * before the banner so the output is machine-parseable (one hex line).
     * compile_overlays.py compares this to the hash from --runtime-include and
     * refuses to build shards with a stale recompiler binary. */
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--codegen-hash") {
            fmt::print("{:08x}\n", (unsigned)PSX_OVERLAY_CODEGEN_HASH);
            return 0;
        }
        if (std::string(argv[i]) == "--overlay-config-hash" && i + 1 < argc) {
            try {
                const auto cfg = PSXRecompV4::load_game_config(argv[i + 1]);
                fmt::print("{:08x}\n",
                           (unsigned)PSXRecompV4::overlay_codegen_config_hash(cfg));
                return 0;
            } catch (const std::exception& e) {
                fmt::print(stderr, "overlay config hash failed: {}\n", e.what());
                return 2;
            }
        }
    }

    fmt::print("PSXRecomp - PlayStation 1 Static Recompiler\n");
    fmt::print("============================================\n\n");

    // ── --config <path> short-form ────────────────────────────────────
    // If --config is provided, all paths come from the TOML and other
    // CLI flags are ignored. Positional form below stays for back-compat.
    std::filesystem::path config_path;
    // --ws-config <path>: load ONLY the [widescreen] site lists from this TOML
    // without treating it as the full game config. Used by compile_overlays.py
    // so overlay compilation gets the widescreen emits (sprite-tag / cull /
    // backdrop) whose addresses live in overlay code — the overlay path is
    // positional (no --config), so this is the channel for those sites.
    std::filesystem::path ws_config_path;
    // --project-root <dir>: the directory to resolve the BIOS profile against
    // when no explicit [recompiler] bios_config is in play. Without it the
    // probe below uses the process CWD, which is wrong for every caller that
    // cannot choose its own CWD — notably compile_overlays.py, which spawns us
    // with cwd = dirname(game.toml). For a PACKAGED config (packaging/release/
    // game.toml) that directory holds neither bios/SCPH1001.toml nor a vendored
    // framework, so every shard died with "no BIOS profile found" and the whole
    // overlay cache silently failed to build (issue #72).
    std::filesystem::path project_root;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
            break;
        }
        if (a.rfind("--config=", 0) == 0) {
            config_path = a.substr(std::string("--config=").size());
            break;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ws-config" && i + 1 < argc) { ws_config_path = argv[i + 1]; break; }
        if (a.rfind("--ws-config=", 0) == 0) {
            ws_config_path = a.substr(std::string("--ws-config=").size());
            break;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--project-root" && i + 1 < argc) { project_root = argv[i + 1]; break; }
        if (a.rfind("--project-root=", 0) == 0) {
            project_root = a.substr(std::string("--project-root=").size());
            break;
        }
    }
    if (!project_root.empty()) {
        std::error_code prec;
        if (!std::filesystem::is_directory(project_root, prec)) {
            fmt::print(stderr,
                "psxrecomp-game: FATAL: --project-root '{}' is not a directory\n",
                project_root.string());
            return 1;
        }
    }

    std::filesystem::path exe_path;
    std::string           extra_funcs_storage;  // lifetime anchor for the .c_str() below
    const char*           extra_funcs_path = nullptr;
    bool                  inspect_mode = false;
    bool                  overlay_mode = false;
    bool                  reachable_discovery = false;
    std::set<uint32_t>    ws_tag_funcs;         // [widescreen] sprite_tag_funcs
    std::set<uint32_t>    ds_funcs;             // [data_shards] funcs
    std::set<uint32_t>    mod_entry_funcs;      // trusted game-mod entry hooks
    std::set<uint32_t>    hot_funcs;            // [recompiler] hot_funcs
    std::set<uint32_t>    load_charge_batch_funcs; // [recompiler] load_charge_batch*
    std::map<uint32_t, std::array<uint32_t, 4>> vsync_query_hle_funcs;
    std::set<uint32_t>    ws_cull_bias, ws_cull_range, ws_cull_a1; // [widescreen.cull]
    std::set<uint32_t>    ws_cull_screen_x;    // [widescreen.cull] screen_x_sites
    std::set<uint32_t>    ws_cull_slti;         // [widescreen.cull] slti_sites
    std::set<uint32_t>    ws_cull_slti_lower;   // [widescreen.cull] slti_lower_sites
    std::set<uint32_t>    ws_cull_bltz;         // [widescreen.cull] bltz_sites
    std::set<uint32_t>    ws_cull_negsub;       // [widescreen.cull] negsub_sites
    std::set<uint32_t>    ws_cull_vxrange;      // [widescreen.cull] vxrange_sites
    std::set<uint32_t>    ws_cull_depth;        // [widescreen.cull] depth_sites
    std::set<uint32_t>    ws_cull_plane_nx;     // [widescreen.cull] plane_nx_sites
    std::set<uint32_t>    ws_cull_xclip_load;   // [widescreen.cull] xclip_load_sites
    std::set<uint32_t>    ws_cull_nclip_keep;   // [widescreen.cull] nclip_keep_sites
    std::set<uint32_t>    ws_cull_branch_keep;  // [widescreen.cull] branch_keep_sites
    std::vector<PSXRecompV4::WidescreenCullKeepSite> ws_cull_keep;
    std::vector<PSXRecompV4::WidescreenCullWidenSite> ws_cull_widen;
    std::vector<PSXRecompV4::WidescreenAngleSite> ws_cull_angle;
    PSXRecompV4::WidescreenAspectConeConfig ws_aspect_cone;
    int                   ws_cull_activation_guard_pixels = 0;
    std::vector<uint32_t> ws_cull_w_imms = { 0x140, 0x141 }; // [widescreen.cull] screen_w_imms
    std::vector<uint32_t> ws_cull_h_imms = { 0xE0, 0xF1 };   // [widescreen.cull] screen_h_imms
    std::set<uint32_t>    ws_backdrop_x;        // [widescreen.backdrop] x_sites
    std::set<uint32_t>    ws_backdrop_unsquash; // [widescreen.backdrop] unsquash_funcs
    bool                  ws_auto_screen_x_cull = false; // [widescreen.cull] auto_screen_x
    std::set<uint32_t>    persist_init_sites;   // [persist_options] init-store hooks (game_options.toml)
    std::vector<PSXRecompV4::RecompilerPatch> instruction_patches;
    std::vector<PSXRecompV4::WidescreenSignedBoundSite> ws_signed_x_bound_sites;
    bool                  ws_auto_backdrop_preload = false; // [widescreen.cull] auto_backdrop
    uint32_t              ws_bg2d_count_site = 0, ws_bg2d_startcol_site = 0,
                          ws_bg2d_startx_site = 0,
                          ws_bg2d_stream_left_site = 0,
                          ws_bg2d_stream_right_site = 0,
                          ws_bg2d_bufbase_site = 0,
                          ws_bg2d_cap_site = 0,
                          ws_bg2d_init_func = 0; // [widescreen.bg2d]
    std::filesystem::path out_dir = "generated";
    uint32_t              configured_text_size = 0;
    std::filesystem::path bios_profile_path;   // [recompiler] bios_config

    if (!config_path.empty()) {
        const auto cfg = PSXRecompV4::load_game_config(config_path);
        exe_path             = cfg.exe_path;
        configured_text_size = cfg.text_size;
        reachable_discovery  = cfg.discovery == "reachable";
        extra_funcs_storage  = cfg.seeds_path.string();
        extra_funcs_path     = extra_funcs_storage.c_str();
        bios_profile_path    = cfg.bios_config_path;
        out_dir              = cfg.out_dir;
        instruction_patches  = cfg.recompiler_patches;
        ws_tag_funcs.insert(cfg.ws_sprite_tag_funcs.begin(),
                            cfg.ws_sprite_tag_funcs.end());
        ds_funcs.insert(cfg.data_shard_funcs.begin(), cfg.data_shard_funcs.end());
        mod_entry_funcs.insert(cfg.mod_function_entry_funcs.begin(),
                               cfg.mod_function_entry_funcs.end());
        hot_funcs.insert(cfg.hot_funcs.begin(), cfg.hot_funcs.end());
        if (cfg.load_charge_batch) {
            load_charge_batch_funcs.insert(cfg.load_charge_batch_funcs.begin(),
                                           cfg.load_charge_batch_funcs.end());
        }
        if (cfg.vsync_query_func)
            vsync_query_hle_funcs[cfg.vsync_query_func] = {
                cfg.vsync_counter_addr, cfg.vsync_gpustat_ptr_addr,
                cfg.vsync_timer1_ptr_addr, cfg.vsync_timer1_cache_addr };
        ws_cull_bias.insert(cfg.ws_cull_bias_sites.begin(), cfg.ws_cull_bias_sites.end());
        ws_cull_range.insert(cfg.ws_cull_range_sites.begin(), cfg.ws_cull_range_sites.end());
        ws_cull_a1.insert(cfg.ws_cull_a1_sites.begin(), cfg.ws_cull_a1_sites.end());
        ws_cull_screen_x.insert(cfg.ws_cull_screen_x_sites.begin(), cfg.ws_cull_screen_x_sites.end());
        ws_cull_slti.insert(cfg.ws_cull_slti_sites.begin(), cfg.ws_cull_slti_sites.end());
        ws_cull_slti_lower.insert(cfg.ws_cull_slti_lower_sites.begin(),
                                  cfg.ws_cull_slti_lower_sites.end());
        ws_cull_bltz.insert(cfg.ws_cull_bltz_sites.begin(), cfg.ws_cull_bltz_sites.end());
        ws_cull_negsub.insert(cfg.ws_cull_negsub_sites.begin(), cfg.ws_cull_negsub_sites.end());
        ws_cull_vxrange.insert(cfg.ws_cull_vxrange_sites.begin(), cfg.ws_cull_vxrange_sites.end());
        ws_cull_depth.insert(cfg.ws_cull_depth_sites.begin(), cfg.ws_cull_depth_sites.end());
        ws_cull_plane_nx.insert(cfg.ws_cull_plane_nx_sites.begin(), cfg.ws_cull_plane_nx_sites.end());
        ws_cull_xclip_load.insert(cfg.ws_cull_xclip_load_sites.begin(), cfg.ws_cull_xclip_load_sites.end());
        ws_cull_nclip_keep.insert(cfg.ws_cull_nclip_keep_sites.begin(), cfg.ws_cull_nclip_keep_sites.end());
        ws_cull_branch_keep.insert(cfg.ws_cull_branch_keep_sites.begin(), cfg.ws_cull_branch_keep_sites.end());
        ws_cull_keep = cfg.ws_cull_keep_sites;
        ws_cull_widen = cfg.ws_cull_widen_sites;
        ws_cull_angle = cfg.ws_cull_angle_sites;
        ws_aspect_cone = cfg.ws_aspect_cone;
        ws_cull_activation_guard_pixels =
            cfg.ws_cull_activation_guard_pixels;
        ws_cull_w_imms = cfg.ws_cull_w_imms;
        ws_cull_h_imms = cfg.ws_cull_h_imms;
        ws_backdrop_x.insert(cfg.ws_backdrop_x_sites.begin(), cfg.ws_backdrop_x_sites.end());
        ws_backdrop_unsquash.insert(cfg.ws_backdrop_unsquash_funcs.begin(), cfg.ws_backdrop_unsquash_funcs.end());
        ws_auto_screen_x_cull = ws_auto_screen_x_cull || cfg.ws_auto_screen_x_cull;
        ws_auto_backdrop_preload = ws_auto_backdrop_preload || cfg.ws_auto_backdrop_preload;
        if (cfg.ws_bg2d_count_site)    ws_bg2d_count_site    = cfg.ws_bg2d_count_site;
        if (cfg.ws_bg2d_startcol_site) ws_bg2d_startcol_site = cfg.ws_bg2d_startcol_site;
        if (cfg.ws_bg2d_startx_site)   ws_bg2d_startx_site   = cfg.ws_bg2d_startx_site;
        if (cfg.ws_bg2d_stream_left_site)  ws_bg2d_stream_left_site  = cfg.ws_bg2d_stream_left_site;
        if (cfg.ws_bg2d_stream_right_site) ws_bg2d_stream_right_site = cfg.ws_bg2d_stream_right_site;
        if (cfg.ws_bg2d_bufbase_site) ws_bg2d_bufbase_site = cfg.ws_bg2d_bufbase_site;
        if (cfg.ws_bg2d_cap_site)     ws_bg2d_cap_site     = cfg.ws_bg2d_cap_site;
        if (cfg.ws_bg2d_init_func)    ws_bg2d_init_func    = cfg.ws_bg2d_init_func;
        ws_signed_x_bound_sites = cfg.ws_signed_x_bound_sites;
        // [persist_options] init-store hook sites live in a dedicated
        // game_options.toml next to game.toml (the game's own native OPTION
        // settings, kept separate from game.toml/settings.toml). Best-effort:
        // a missing/bad file just means no persistence hooks this build.
        try {
            std::filesystem::path go_path =
                std::filesystem::path(config_path).parent_path() / "game_options.toml";
            const auto goc = PSXRecompV4::load_game_options(go_path);
            for (const auto& o : goc.options)
                if (o.init_store_pc) persist_init_sites.insert(o.init_store_pc);
        } catch (const std::exception& e) {
            fmt::print(stderr, "  game_options.toml ignored: {}\n", e.what());
        }
        fmt::print("config:         {}\n", config_path.string());
        fmt::print("  exe         = {}\n", exe_path.string());
        fmt::print("  seeds       = {}\n", extra_funcs_storage);
        fmt::print("  out_dir     = {}\n", out_dir.string());
        if (!ws_tag_funcs.empty())
            fmt::print("  ws_tag_funcs= {}\n", ws_tag_funcs.size());
        fmt::print("\n");
    } else {
        exe_path = argv[1];
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if ((arg == "--extra-funcs" || arg == "--seeds") && i + 1 < argc) {
                extra_funcs_path = argv[++i];
            } else if (arg == "--out-dir" && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (arg == "--strict") {
                /* The PS-X EXE path is fail-loud by default; accepted for parity
                 * with psxrecomp-bios and project scripts. */
            } else if (arg == "--inspect") {
                inspect_mode = true;
            } else if (arg == "--project-root" && i + 1 < argc) {
                /* Already parsed in the pre-scan above (the BIOS-profile probe
                 * needs it before this loop runs). Consume the value here so it
                 * is never mistaken for another flag's argument. */
                ++i;
            } else if (arg == "--overlay") {
                /* Overlay-compilation contract: this input is a runtime-captured
                 * overlay with execution evidence, so discovery is evidence-scoped
                 * (no whole-byte sweep) and branch/jump-table targets stay as
                 * in-parent labels. Set unconditionally by compile_overlays.py for
                 * every overlay input — never a human toggle. */
                overlay_mode = true;
            }
        }
    }

    // --ws-config: merge the [widescreen] site lists from a side config WITHOUT
    // adopting its exe/paths. The overlay path is positional (no --config), so
    // this is how overlay compilation receives the widescreen emits whose
    // addresses live in overlay code (backdrop screenX, and any sprite-tag /
    // cull sites that resolve there). No-op when --config already provided the
    // sites (same TOML) — std::set insert dedupes.
    if (!ws_config_path.empty()) {
        const auto wscfg = PSXRecompV4::load_game_config(ws_config_path);
        ws_tag_funcs.insert(wscfg.ws_sprite_tag_funcs.begin(), wscfg.ws_sprite_tag_funcs.end());
        mod_entry_funcs.insert(wscfg.mod_function_entry_funcs.begin(),
                               wscfg.mod_function_entry_funcs.end());
        ws_cull_bias.insert(wscfg.ws_cull_bias_sites.begin(), wscfg.ws_cull_bias_sites.end());
        ws_cull_range.insert(wscfg.ws_cull_range_sites.begin(), wscfg.ws_cull_range_sites.end());
        ws_cull_a1.insert(wscfg.ws_cull_a1_sites.begin(), wscfg.ws_cull_a1_sites.end());
        ws_cull_screen_x.insert(wscfg.ws_cull_screen_x_sites.begin(), wscfg.ws_cull_screen_x_sites.end());
        ws_cull_slti.insert(wscfg.ws_cull_slti_sites.begin(), wscfg.ws_cull_slti_sites.end());
        ws_cull_slti_lower.insert(wscfg.ws_cull_slti_lower_sites.begin(),
                                  wscfg.ws_cull_slti_lower_sites.end());
        ws_cull_bltz.insert(wscfg.ws_cull_bltz_sites.begin(), wscfg.ws_cull_bltz_sites.end());
        ws_cull_negsub.insert(wscfg.ws_cull_negsub_sites.begin(), wscfg.ws_cull_negsub_sites.end());
        ws_cull_vxrange.insert(wscfg.ws_cull_vxrange_sites.begin(), wscfg.ws_cull_vxrange_sites.end());
        ws_cull_depth.insert(wscfg.ws_cull_depth_sites.begin(), wscfg.ws_cull_depth_sites.end());
        ws_cull_plane_nx.insert(wscfg.ws_cull_plane_nx_sites.begin(), wscfg.ws_cull_plane_nx_sites.end());
        ws_cull_xclip_load.insert(wscfg.ws_cull_xclip_load_sites.begin(), wscfg.ws_cull_xclip_load_sites.end());
        ws_cull_nclip_keep.insert(wscfg.ws_cull_nclip_keep_sites.begin(), wscfg.ws_cull_nclip_keep_sites.end());
        ws_cull_branch_keep.insert(wscfg.ws_cull_branch_keep_sites.begin(), wscfg.ws_cull_branch_keep_sites.end());
        if (ws_cull_keep.empty()) ws_cull_keep = wscfg.ws_cull_keep_sites;
        if (ws_cull_widen.empty()) ws_cull_widen = wscfg.ws_cull_widen_sites;
        if (ws_cull_angle.empty()) ws_cull_angle = wscfg.ws_cull_angle_sites;
        if (ws_aspect_cone.sites.empty())
            ws_aspect_cone = wscfg.ws_aspect_cone;
        ws_cull_activation_guard_pixels =
            wscfg.ws_cull_activation_guard_pixels;
        ws_cull_w_imms = wscfg.ws_cull_w_imms;
        ws_cull_h_imms = wscfg.ws_cull_h_imms;
        ws_backdrop_x.insert(wscfg.ws_backdrop_x_sites.begin(), wscfg.ws_backdrop_x_sites.end());
        ws_backdrop_unsquash.insert(wscfg.ws_backdrop_unsquash_funcs.begin(), wscfg.ws_backdrop_unsquash_funcs.end());
        ws_auto_screen_x_cull = ws_auto_screen_x_cull || wscfg.ws_auto_screen_x_cull;
        ws_auto_backdrop_preload = ws_auto_backdrop_preload || wscfg.ws_auto_backdrop_preload;
        PSXRecompV4::merge_recompiler_patches(
            instruction_patches, wscfg.recompiler_patches);
        ws_signed_x_bound_sites.insert(ws_signed_x_bound_sites.end(),
                                       wscfg.ws_signed_x_bound_sites.begin(),
                                       wscfg.ws_signed_x_bound_sites.end());
        if (wscfg.ws_bg2d_init_func) ws_bg2d_init_func = wscfg.ws_bg2d_init_func;
        fmt::print("ws-config:      {} (backdrop_x sites={}, unsquash funcs={})\n",
                   ws_config_path.string(), ws_backdrop_x.size(), ws_backdrop_unsquash.size());
    }

    /* Resolve the BIOS profile this game builds against and activate its
     * address model for codegen (jump tables inside relocated BIOS code
     * windows fold through it — see code_generator.cpp ram_to_rom). Order:
     * explicit [recompiler] bios_config; else the SCPH1001 profile in-repo
     * (framework checkout) or under the psxrecomp/ submodule (game repo).
     * No profile -> fatal: there are no built-in windows. */
    static PSXRecompV4::BiosAddressModel s_bios_model;
    {
        /* The directory the profile probe walks. --project-root when given,
         * else the CWD (unchanged legacy behaviour). Named in every failure
         * below, because "no BIOS profile found" without the directory it
         * searched sends you looking for an unset key that is in fact set —
         * or, worse, reads as a config error when it is a CWD error. */
        const std::filesystem::path search_root =
            project_root.empty() ? std::filesystem::path(".") : project_root;

        std::filesystem::path prof = bios_profile_path;
        /* An explicit bios_config that does not resolve is a different failure
         * from having none at all — say which, or the message sends you looking
         * for an unset key that is in fact set. A relative bios_config is
         * documented as relative to the game project root, so resolve it there
         * first when we were told what the root is; fall back to the bare path
         * so callers that already relied on CWD keep working. */
        if (!prof.empty() && !std::filesystem::exists(prof)) {
            std::filesystem::path rooted;
            if (!project_root.empty() && prof.is_relative()) {
                rooted = project_root / prof;
            }
            if (!rooted.empty() && std::filesystem::exists(rooted)) {
                prof = rooted;
            } else {
                fmt::print(stderr,
                    "psxrecomp-game: FATAL: [recompiler] bios_config = '{}' does "
                    "not exist (path is relative to the game project root; "
                    "searched '{}')\n",
                    prof.string(),
                    std::filesystem::absolute(search_root).string());
                return 1;
            }
        }
        if (prof.empty()) {
            if (std::filesystem::exists(search_root / "bios/SCPH1001.toml")) {
                prof = search_root / "bios/SCPH1001.toml"; /* framework checkout */
            } else {
                /* Game repo vendoring the framework. Do NOT assume the
                 * directory is named "psxrecomp": ApeEscapeRecomp vendors it as
                 * "psxrecomp-v4", and hardcoding the name made regeneration
                 * fail outright there. Probe one level for any vendored
                 * framework, sorted so the pick is deterministic. */
                std::vector<std::filesystem::path> found;
                std::error_code ec;
                for (const auto& de :
                         std::filesystem::directory_iterator(search_root, ec)) {
                    if (ec) break;
                    if (!de.is_directory(ec) || ec) continue;
                    auto cand = de.path() / "bios" / "SCPH1001.toml";
                    if (std::filesystem::exists(cand)) found.push_back(cand);
                }
                std::sort(found.begin(), found.end());
                if (!found.empty()) prof = found.front();
            }
        }
        if (prof.empty()) {
            fmt::print(stderr,
                "psxrecomp-game: FATAL: no BIOS profile found. Searched '{}' "
                "for bios/SCPH1001.toml and <framework>/bios/SCPH1001.toml. "
                "Fix by setting [recompiler] bios_config in game.toml, passing "
                "--project-root <dir> (the framework or game-project root), or "
                "running from a root that contains one of those paths.\n",
                std::filesystem::absolute(search_root).string());
            return 1;
        }
        const auto bios_cfg = PSXRecompV4::load_bios_config(prof);
        s_bios_model = PSXRecompV4::BiosAddressModel::from_config(bios_cfg);
        PSXRecomp::CodeGenerator::set_bios_address_model(&s_bios_model);
    }

    // Parse the PS1-EXE file
    std::string error_msg;
    fmt::print("Parsing PS1-EXE: {}\n", exe_path.string());

    auto exe = PSXRecomp::PS1ExeParser::parse_file(exe_path, error_msg);

    if (!exe.has_value()) {
        fmt::print(stderr, "Failed to parse PS1-EXE: {}\n", error_msg);
        return 1;
    }

    // [game].text_size is the title-owned static-analysis bound. It may trim a
    // verified data/padding tail, but it may never widen the parsed payload or
    // exclude the executable entry. Runtime disc loading remains unchanged.
    if (!config_path.empty()) {
        const uint32_t parsed_size = exe->header.file_size;
        std::string bound_error;
        if (!PSXRecomp::apply_static_analysis_bound(
                *exe, configured_text_size, bound_error)) {
            fmt::print(stderr, "Invalid static analysis bound: {}\n", bound_error);
            return 1;
        }
        if (exe->header.file_size < parsed_size) {
            fmt::print("Static analysis bound: 0x{:X} bytes from game.text_size "
                       "(PS-X EXE header: 0x{:X})\n",
                       exe->header.file_size, parsed_size);
        }
    }

    // Patch the bounded image before discovery/CFG analysis so replacements of
    // control-flow instructions cannot disagree with the generated graph.
    PSXRecompV4::apply_recompiler_patches_to_executable(
        *exe, instruction_patches, overlay_mode);

    fmt::print("✓ Successfully parsed PS1-EXE!\n\n");

    // Print header information
    fmt::print("PS1-EXE Header Info:\n");
    fmt::print("  Entry Point:    0x{:08X}\n", exe->header.initial_pc);
    fmt::print("  Load Address:   0x{:08X}\n", exe->header.load_address);
    fmt::print("  Code Size:      {} bytes ({} KB)\n",
               exe->header.file_size, exe->header.file_size / 1024);
    fmt::print("  End Address:    0x{:08X}\n", exe->end_address());
    fmt::print("  Global Pointer: 0x{:08X}\n", exe->header.initial_gp);
    fmt::print("  Stack Pointer:  0x{:08X}\n\n", exe->header.initial_sp);

    // Validate entry point
    if (!exe->header.entry_in_range()) {
        fmt::print(stderr, "⚠ Warning: Entry point 0x{:08X} is outside loaded code range!\n\n",
                   exe->header.initial_pc);
    }

    if (inspect_mode) {
    // Disassemble first 20 instructions from entry point
    fmt::print("Disassembly at Entry Point (0x{:08X}):\n", exe->header.initial_pc);
    fmt::print("---------------------------------------\n");

    uint32_t current_addr = exe->header.initial_pc;
    const int num_instructions = 20;

    for (int i = 0; i < num_instructions; i++) {
        auto word_opt = exe->read_word(current_addr);

        if (!word_opt.has_value()) {
            fmt::print("  [End of code or read error]\n");
            break;
        }

        uint32_t instr_word = *word_opt;

        // Use Rabbitizer to disassemble the instruction
        // Note: Rabbitizer expects big-endian, PS1 is little-endian, so the word is already in correct format
        rabbitizer::InstructionCpu instr(instr_word, current_addr);

        // Get disassembly (parameter is flags, 0 = default)
        std::string disasm = instr.disassemble(0);

        // Print address, hex, and disassembly
        fmt::print("  {:08X}:  {:08X}  {}\n", current_addr, instr_word, disasm);

        current_addr += 4;
    }

    fmt::print("\n");
    fmt::print("✓ Disassembly complete!\n");

    // Instruction frequency analysis
    fmt::print("\n");
    fmt::print("Analyzing instruction frequency...\n");

    std::map<std::string, int> instr_freq;
    std::map<uint32_t, int> opcode_freq;
    std::map<uint32_t, int> special_funct_freq;

    uint32_t total_instructions = 0;
    uint32_t addr = exe->header.load_address;
    while (addr < exe->end_address()) {
        auto word_opt = exe->read_word(addr);
        if (!word_opt.has_value()) break;

        uint32_t instr_word = *word_opt;
        rabbitizer::InstructionCpu instr(instr_word, addr);
        std::string mnemonic = instr.getOpcodeName();

        instr_freq[mnemonic]++;
        total_instructions++;

        // Track opcode distribution
        uint32_t opcode = (instr_word >> 26) & 0x3F;
        opcode_freq[opcode]++;

        // Track SPECIAL function codes
        if (opcode == 0x00) {
            uint32_t funct = instr_word & 0x3F;
            special_funct_freq[funct]++;
        }

        addr += 4;
    }

    fmt::print("Total instructions: {}\n\n", total_instructions);

    // Sort by frequency and print top 20
    std::vector<std::pair<std::string, int>> sorted_freq(instr_freq.begin(), instr_freq.end());
    std::sort(sorted_freq.begin(), sorted_freq.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    fmt::print("=== Top 20 Most Common Instructions ===\n\n");
    fmt::print("{:4s}  {:12s}  {:10s}  {:s}\n", "Rank", "Instruction", "Count", "Percentage");
    fmt::print("-----------------------------------------------------\n");

    for (size_t i = 0; i < std::min(size_t(20), sorted_freq.size()); i++) {
        const auto& [instr_name, count] = sorted_freq[i];
        double pct = 100.0 * count / total_instructions;
        fmt::print("{:4d}  {:12s}  {:10d}  {:5.2f}%\n", i + 1, instr_name, count, pct);
    }

    fmt::print("\n");
    }

    // Function boundary detection
    fmt::print("Performing function analysis...\n");

    std::vector<uint32_t> exact_entries;
    exact_entries.push_back(exe->header.initial_pc);

    // Explicit entry points: functions called through pointers but not
    // automatically detected by the heuristic-based function analysis.
    // Pass game-specific entry points via --extra-funcs file.

    /* Load extra function addresses from a file (one hex address per line).
     * Lines of the form `interior 0xXXXXXXXX` mark dispatch-proven interior
     * addresses: alias candidates, never walk roots. `hosted_interior TARGET
     * HOST` is a narrow cross-variant identity assertion: TARGET may alias only
     * the explicitly rooted HOST, and only when TARGET is a syntactic block
     * leader in HOST's current-byte range. The shard publisher additionally
     * requires both emitted identities to match a previously CRC-validated host
     * exactly before the result can reach the runtime. Lines of the form
     * `dispatch_root 0xXXXXXXXX` mark classifier-proven dispatch roots
     * WITHOUT a callable boundary (the kernel install-slot class, e.g. RAM
     * 0xCF0): the static recompiler's install-slot hooks tail-dispatch into
     * exactly these PCs, so they are real execution roots even though no
     * prologue / preceding jr $ra exists. They are trusted walk roots and
         * exempt from the overlay-mode boundary re-check below.  Lines of the
         * form `call_root 0xXXXXXXXX` carry the same mechanical trust for a
         * statically proven direct or constant-register call/tail-call target.
     *
     * Seeds are accepted within the loaded image's own bounds — overlay mode
     * wraps arbitrary regions (kernel RAM at 0x80000000, overlays at
     * 0x800E7000, ...), so a hardcoded game-RAM window would silently drop
     * valid seeds. */
    std::vector<uint32_t> file_seeds;
    std::vector<uint32_t> interior_seeds;
    std::map<uint32_t, uint32_t> hosted_interior_seeds;
    std::set<uint32_t> hosted_interior_hosts;
    constexpr size_t kHostedInteriorTargetCap = 4096;
    constexpr size_t kHostedInteriorHostCap = 512;
    std::set<uint32_t>    trusted_root_seeds;
    std::set<uint32_t>    trusted_call_root_seeds;
    std::vector<std::pair<uint32_t, uint32_t>> producer_ranges;
    std::set<uint32_t> cross_call_allow;
    if (extra_funcs_path) {
        std::ifstream ef(extra_funcs_path);
        if (ef.is_open()) {
            const uint32_t seed_lo = exe->header.load_address;
            const uint32_t seed_hi = exe->end_address();
            std::string line;
            while (std::getline(ef, line)) {
                if (line.empty() || line[0] == '#') continue;
                if (line.rfind("producer_range", 0) == 0) {
                    std::istringstream in(line);
                    std::string tag, lo_text, hi_text;
                    in >> tag >> lo_text >> hi_text;
                    uint32_t lo = static_cast<uint32_t>(
                        std::strtoul(lo_text.c_str(), nullptr, 16));
                    uint32_t hi = static_cast<uint32_t>(
                        std::strtoul(hi_text.c_str(), nullptr, 16));
                    if (lo_text.empty() || hi_text.empty() ||
                        lo < seed_lo || lo >= hi || hi > seed_hi) {
                        fmt::print(stderr, "ERROR: invalid producer_range: {}\n", line);
                        return 1;
                    }
                    producer_ranges.emplace_back(lo, hi);
                    continue;
                }
                if (line.rfind("cross_call_allow", 0) == 0) {
                    const char* p = line.c_str() + 16;
                    uint32_t addr = static_cast<uint32_t>(
                        std::strtoul(p, nullptr, 16));
                    if (addr < seed_lo || addr >= seed_hi) {
                        fmt::print(stderr, "ERROR: invalid cross_call_allow: {}\n", line);
                        return 1;
                    }
                    cross_call_allow.insert(addr);
                    continue;
                }
                if (line.rfind("retained_alias", 0) == 0) {
                    fmt::print("  ignoring legacy retained_alias; old host "
                               "ranges are never reused: {}\n", line);
                    continue;
                }
                if (line.rfind("hosted_interior", 0) == 0) {
                    std::istringstream in(line);
                    std::string tag, target_text, host_text, trailing;
                    in >> tag >> target_text >> host_text >> trailing;
                    auto parse_hex_u32 = [](const std::string& text,
                                            uint32_t& value) -> bool {
                        if (text.empty()) return false;
                        char* end = nullptr;
                        errno = 0;
                        unsigned long long parsed =
                            std::strtoull(text.c_str(), &end, 16);
                        if (errno != 0 || end == text.c_str() || *end != '\0' ||
                            parsed > 0xFFFFFFFFull) return false;
                        value = static_cast<uint32_t>(parsed);
                        return true;
                    };
                    uint32_t target = 0, host = 0;
                    if (tag != "hosted_interior" || !trailing.empty() ||
                        !parse_hex_u32(target_text, target) ||
                        !parse_hex_u32(host_text, host) ||
                        (target & 3u) != 0u || (host & 3u) != 0u ||
                        target < seed_lo || target >= seed_hi ||
                        host < seed_lo || host >= seed_hi || target == host) {
                        fmt::print(stderr, "ERROR: invalid hosted_interior: {}\n", line);
                        return 1;
                    }
                    auto prior = hosted_interior_seeds.find(target);
                    if (prior != hosted_interior_seeds.end() &&
                        prior->second != host) {
                        fmt::print(stderr, "ERROR: conflicting hosted_interior "
                                   "hosts for 0x{:08X}\n", target);
                        return 1;
                    }
                    hosted_interior_seeds.emplace(target, host);
                    hosted_interior_hosts.insert(host);
                    if (hosted_interior_seeds.size() >
                            kHostedInteriorTargetCap ||
                        hosted_interior_hosts.size() >
                            kHostedInteriorHostCap) {
                        fmt::print(stderr, "ERROR: hosted_interior capacity "
                                   "exceeded (targets {} / {}, hosts {} / {})\n",
                                   hosted_interior_seeds.size(),
                                   kHostedInteriorTargetCap,
                                   hosted_interior_hosts.size(),
                                   kHostedInteriorHostCap);
                        return 1;
                    }
                    continue;
                }
                bool interior = false;
                bool trusted_root = false;
                bool trusted_call_root = false;
                const char* p = line.c_str();
                if (line.rfind("interior", 0) == 0) {
                    interior = true;
                    p += 8;
                } else if (line.rfind("dispatch_root", 0) == 0) {
                    trusted_root = true;
                    p += 13;
                } else if (line.rfind("call_root", 0) == 0) {
                    trusted_call_root = true;
                    p += 9;
                }
                uint32_t addr = (uint32_t)std::strtoul(p, nullptr, 16);
                if (addr >= seed_lo && addr < seed_hi) {
                    if (interior) {
                        interior_seeds.push_back(addr);
                    } else {
                        if (trusted_root) trusted_root_seeds.insert(addr);
                        if (trusted_call_root) trusted_call_root_seeds.insert(addr);
                        file_seeds.push_back(addr);
                        exact_entries.push_back(addr);
                    }
                }
            }
            std::sort(producer_ranges.begin(), producer_ranges.end());
            for (size_t i = 1; i < producer_ranges.size(); i++) {
                if (producer_ranges[i - 1].second > producer_ranges[i].first) {
                    fmt::print(stderr, "ERROR: overlapping producer_range entries\n");
                    return 1;
                }
            }
            for (uint32_t addr : cross_call_allow) {
                bool contained = false;
                for (const auto& range : producer_ranges) {
                    if (addr >= range.first && addr < range.second) {
                        contained = true;
                        break;
                    }
                }
                if (!contained) {
                    fmt::print(stderr,
                               "ERROR: cross_call_allow 0x{:08X} is outside "
                               "producer ranges\n", addr);
                    return 1;
                }
            }
            fmt::print("Loaded {} extra function addresses ({} interior, "
                       "{} hosted-interior, {} dispatch-root, {} call-root, {} producer ranges, "
                       "{} cross-call allows) from {}\n",
                       file_seeds.size() + interior_seeds.size() +
                           hosted_interior_seeds.size(),
                       interior_seeds.size(), hosted_interior_seeds.size(),
                       trusted_root_seeds.size(),
                       trusted_call_root_seeds.size(),
                       producer_ranges.size(), cross_call_allow.size(),
                       extra_funcs_path);
        } else {
            fmt::print("WARNING: Cannot open extra-funcs file: {}\n", extra_funcs_path);
        }
    }

    PSXRecomp::FunctionAnalysisResult analysis_result;

    if (overlay_mode || reachable_discovery) {
        // ── Exact-entry mode: partition seeds into walk roots and interior
        // alias candidates. `interior`-marked seeds (classifier-proven
        // dispatch targets without a callable boundary) are NEVER walk roots —
        // a root inside a host function hard-caps (truncates) it, the
        // mid-function-seed softlock class. Unmarked seeds are re-verified
        // against the same boundary rule as defense in depth.
        const uint32_t exe_lo = exe->header.load_address;
        auto read_w = [&](uint32_t a) -> uint32_t {
            auto w = exe->read_word(a);
            return w.has_value() ? *w : 0u;
        };
        auto dense_local_pointer_table = [&](uint32_t a) -> bool {
            for (uint32_t i = 0; i < 3u; i++) {
                auto value = exe->read_word(a + i * 4u);
                if (!value.has_value() || (*value & 3u) != 0u ||
                    *value < exe_lo || *value >= exe->end_address()) {
                    return false;
                }
            }
            return true;
        };
        auto callable_boundary = [&](uint32_t a) -> bool {
            uint32_t w = read_w(a);
            if (!PSXRecomp::FunctionAnalyzer::is_valid_mips_word(w)) return false;
            if (a == exe_lo) return true;
            if (a >= exe_lo + 8 && read_w(a - 8) == 0x03E00008u) return true;
            // Normal-mode discovery already skips alignment padding after a
            // return when it infers a frameless function start. Preserve that
            // evidence here. Requiring the next entry to be exactly jr-ra+8
            // demoted legitimate Psy-Q leaf routines separated by one to six
            // padding NOPs to orphan interiors, so exact-entry analysis emitted
            // no function at all. The delay slot (jr+4) may be non-NOP; only
            // words after it and before the proposed entry must be padding.
            bool padding_only = true;
            for (uint32_t back = 12; back <= 32 && a >= exe_lo + back;
                 back += 4) {
                if (read_w(a - back + 8) != 0u) padding_only = false;
                if (!padding_only) break;
                if (read_w(a - back) == 0x03E00008u) return true;
            }
            int32_t frame = 0;
            return PSXRecomp::FunctionAnalyzer::is_prologue(w, frame) &&
                   !(a >= exe_lo + 4 &&
                     PSXRecomp::FunctionAnalyzer::is_branch_or_jump(read_w(a - 4)));
        };

        std::set<uint32_t> roots;
        std::set<uint32_t> interior;
        std::set<uint32_t> hosted_targets;
        for (const auto& seed : hosted_interior_seeds)
            hosted_targets.insert(seed.first);
        for (uint32_t a : exact_entries) {
            if (hosted_targets.count(a)) {
                // A fragment PS-X header needs one initial PC, but a trusted
                // hosted interior must never become a walk root through it.
                continue;
            } else if (reachable_discovery && a == exe->header.initial_pc) {
                // The PS-X EXE header is direct execution evidence for the
                // main entry even when it uses a nonstandard prologue.
                roots.insert(a);
            } else if (trusted_root_seeds.count(a)) {
                /* Classifier-proven dispatch root (install-slot class): the
                 * static code tail-dispatches into this PC, so it is a real
                 * execution root despite having no callable boundary. */
                fmt::print("  seed 0x{:08X} accepted as dispatch root "
                           "(install-slot class, no boundary evidence)\n", a);
                roots.insert(a);
            } else if (trusted_call_root_seeds.count(a) &&
                       !dense_local_pointer_table(a)) {
                fmt::print("  seed 0x{:08X} accepted as static call root "
                           "(direct/constant-register target)\n", a);
                roots.insert(a);
            } else if (callable_boundary(a)) {
                roots.insert(a);
            } else {
                fmt::print("  seed 0x{:08X} fails the boundary check — "
                           "treating as interior alias candidate\n", a);
                interior.insert(a);
            }
        }
        for (uint32_t a : interior_seeds) interior.insert(a);

        auto containing = [&](uint32_t a) -> const PSXRecomp::Function* {
            const PSXRecomp::Function* best = nullptr;
            for (const auto& f : analysis_result.functions) {
                if (a >= f.start_addr && a < f.end_addr) { best = &f; break; }
            }
            return best;
        };

        // Interior candidates attach only to functions the EVIDENCE-walked
        // root set already owns. No backward-boundary guessing: a false
        // prologue/jr-ra in embedded data mints a root that decodes data as
        // code (audit-fatal). Orphans stay with the interpreter — it
        // self-heals via recapture once real call evidence appears.
        {
            PSXRecomp::FunctionAnalyzer analyzer(*exe);
            std::vector<uint32_t> roots_vec(roots.begin(), roots.end());
            analysis_result = analyzer.analyze_exact_entries(
                roots_vec, producer_ranges, cross_call_allow);
        }

        std::vector<AliasEntry> alias_entries;
        std::set<uint32_t> alias_seen;
        for (uint32_t a : interior) {
            const PSXRecomp::Function* host = containing(a);
            if (host && a == host->start_addr) continue;  // became a real entry
            if (host && analysis_result.exact_reachable_pcs.count(a)) {
                if (alias_seen.insert(a).second)
                    alias_entries.push_back({a, host->start_addr, host->end_addr});
            } else {
                fmt::print("  WARNING: interior seed 0x{:08X} has no host "
                           "function — left to the interpreter\n", a);
            }
        }

        // Cross-variant propagation never guesses a new walk. The caller names
        // a current-byte host that was already CRC-valid in the cache, and the
        // recompiler requires that exact root plus a syntactic block boundary
        // in the host's current-byte range. This block test alone is NOT proof
        // of reachability: the publisher must compare the emitted host+alias
        // CRC/range identities with the preselected cached owner before use.
        PSXRecomp::ControlFlowAnalyzer hosted_cfg_analyzer(*exe);
        std::map<uint32_t, PSXRecomp::ControlFlowGraph> hosted_cfgs;
        for (const auto& seed : hosted_interior_seeds) {
            uint32_t target = seed.first;
            uint32_t requested_host = seed.second;
            const PSXRecomp::Function* host = nullptr;
            for (const auto& function : analysis_result.functions) {
                if (function.start_addr == requested_host) {
                    host = &function;
                    break;
                }
            }
            bool explicitly_rooted = roots.count(requested_host) != 0;
            bool producer_ok = host &&
                (host->producer_lo == 0 ||
                 (target >= host->producer_lo && target < host->producer_hi));
            bool block_ok = false;
            if (host && producer_ok && target > host->start_addr &&
                target < host->end_addr) {
                auto inserted = hosted_cfgs.emplace(
                    host->start_addr, PSXRecomp::ControlFlowGraph{});
                if (inserted.second)
                    inserted.first->second = hosted_cfg_analyzer.analyze_function(*host);
                block_ok = inserted.first->second.blocks.count(target) != 0;
            }
            if (host && explicitly_rooted && producer_ok && block_ok) {
                if (alias_seen.insert(target).second)
                    alias_entries.push_back(
                        {target, host->start_addr, host->end_addr});
            } else {
                fmt::print("  WARNING: hosted interior 0x{:08X} rejected for "
                           "host 0x{:08X} (host={}, rooted={}, producer={}, block={})\n",
                           target, requested_host, host ? "yes" : "no",
                           explicitly_rooted ? "yes" : "no",
                           producer_ok ? "yes" : "no", block_ok ? "yes" : "no");
            }
        }

        // Range-ownership completeness uses only analyzer-proven transfers from
        // reachable instructions. The analyzer also revalidates each target's
        // host against the final root partition and producer boundaries.
        size_t proven_alias_added = 0;
        for (const auto& absorbed : analysis_result.absorbed_entries) {
            if (!alias_seen.insert(absorbed.addr).second) continue;
            alias_entries.push_back(
                {absorbed.addr, absorbed.host_start, absorbed.host_end});
            proven_alias_added++;
        }
        if (proven_alias_added)
            fmt::print("  +{} analyzer-proven absorbed alias entries "
                       "(final range ownership)\n", proven_alias_added);

        materialize_alias_groups(analysis_result, alias_entries);
        fmt::print("Exact-entry alias entries emitted: {}\n\n", alias_entries.size());
    } else {
        // ── Iterative discovery: seed classification + static data-table scan ──
        //
        // Every candidate entry (explicit seed or scanned data pointer) is
        // classified against the current function set:
        //   * matches a function start            → already covered
        //   * INSIDE a function's range           → ALIAS entry (overlapping
        //     emission; the host is never capped/truncated — forcing such an
        //     entry is the mid-function-seed softlock class)
        //   * in a gap between functions          → forced entry (explicit
        //     seeds are trusted; scanned pointers additionally need boundary
        //     evidence: a jr $ra two slots earlier or a non-delay-slot
        //     prologue)
        //
        // The data scan walks every word of the image OUTSIDE code-function
        // ranges (pointer tables live in data regions) and collects words that
        // point at valid instructions inside code-function ranges. Promotions
        // change function boundaries, so iterate to a fixed point.
        std::vector<AliasEntry> alias_entries;

        std::set<uint32_t> forced;
        forced.insert(exe->header.initial_pc);
        const uint32_t exe_lo = exe->header.load_address;
        const uint32_t exe_hi = exe->end_address();
        auto read_w = [&](uint32_t a) -> uint32_t {
            auto w = exe->read_word(a);
            return w.has_value() ? *w : 0u;
        };

        const int kMaxDiscoveryIters = 6;
        for (int iter = 0; iter < kMaxDiscoveryIters; ++iter) {
            PSXRecomp::FunctionAnalyzer analyzer(*exe);
            for (uint32_t a : forced) analyzer.add_forced_entry(a);
            analysis_result = analyzer.analyze();

            std::vector<const PSXRecomp::Function*> code_funcs;
            for (const auto& f : analysis_result.functions) {
                if (!f.is_data_section) code_funcs.push_back(&f);
            }
            auto containing = [&](uint32_t a) -> const PSXRecomp::Function* {
                auto it = std::upper_bound(code_funcs.begin(), code_funcs.end(), a,
                    [](uint32_t v, const PSXRecomp::Function* f) { return v < f->start_addr; });
                if (it == code_funcs.begin()) return nullptr;
                --it;
                return (a >= (*it)->start_addr && a < (*it)->end_addr) ? *it : nullptr;
            };

            struct Candidate { uint32_t addr; bool trusted; bool table_evidence; };
            std::vector<Candidate> candidates;
            for (uint32_t s : file_seeds) candidates.push_back({s, true, true});
            for (uint32_t s : interior_seeds) candidates.push_back({s, true, true});
            uint32_t scanned_words = 0;
            auto is_text_ptr = [&](uint32_t v) {
                return (v & 3u) == 0 && v >= exe_lo && v < exe_hi && containing(v) != nullptr;
            };
            for (uint32_t p = exe_lo; p + 4 <= exe_hi; p += 4) {
                if (containing(p)) continue;  // instruction word, not a data word
                uint32_t w = read_w(p);
                if ((w & 3u) || w < exe_lo || w >= exe_hi) continue;
                // Table evidence: an adjacent data word that is also a text
                // pointer. Interior handler entries live in dense pointer
                // tables; a lone pointer-shaped data word is too weak to mint
                // an alias from (random data aliases into loose code ranges).
                bool table_evidence =
                    (p >= exe_lo + 4 && is_text_ptr(read_w(p - 4))) ||
                    (p + 8 <= exe_hi && is_text_ptr(read_w(p + 4)));
                candidates.push_back({w, false, table_evidence});
                scanned_words++;
            }

            alias_entries.clear();
            std::set<uint32_t> alias_seen;
            bool promoted_new = false;
            for (const auto& [a, trusted, table_evidence] : candidates) {
                if ((a & 3u) || a < exe_lo || a >= exe_hi) continue;
                const PSXRecomp::Function* host = containing(a);
                if (host && a == host->start_addr) continue;  // already an entry
                uint32_t w = read_w(a);
                if (!PSXRecomp::FunctionAnalyzer::is_valid_mips_word(w)) continue;
                if (host) {
                    if ((trusted || table_evidence) && alias_seen.insert(a).second) {
                        alias_entries.push_back({a, host->start_addr, host->end_addr});
                    }
                    continue;
                }
                bool promote = trusted;
                if (!promote) {
                    bool after_ret = (a >= exe_lo + 8) && (read_w(a - 8) == 0x03E00008u);
                    int32_t frame = 0;
                    bool prologue =
                        PSXRecomp::FunctionAnalyzer::is_prologue(w, frame) &&
                        !(a >= exe_lo + 4 &&
                          PSXRecomp::FunctionAnalyzer::is_branch_or_jump(read_w(a - 4)));
                    promote = after_ret || prologue;
                }
                if (promote && forced.insert(a).second) promoted_new = true;
            }

            fmt::print("Discovery iteration {}: {} data pointers scanned, "
                       "{} forced entries, {} alias entries\n",
                       iter + 1, scanned_words, forced.size(), alias_entries.size());
            if (!promoted_new) break;
        }

        materialize_alias_groups(analysis_result, alias_entries);
        fmt::print("Alias entries emitted: {}\n\n", alias_entries.size());
    }

    // Print summary statistics
    fmt::print("\n=== Function Analysis Summary ===\n\n");
    fmt::print("Total Functions: {}\n", analysis_result.functions.size());
    fmt::print("Code Coverage:   {} KB analyzed\n",
               (exe->end_address() - exe->header.load_address) / 1024);

    // Count functions with prologues/epilogues
    int with_prologue = 0;
    int with_epilogue = 0;
    int with_both = 0;

    for (const auto& func : analysis_result.functions) {
        if (func.has_prologue) with_prologue++;
        if (func.has_epilogue) with_epilogue++;
        if (func.has_prologue && func.has_epilogue) with_both++;
    }

    fmt::print("With Prologue:   {} ({:.1f}%)\n", with_prologue,
               100.0 * with_prologue / analysis_result.functions.size());
    fmt::print("With Epilogue:   {} ({:.1f}%)\n", with_epilogue,
               100.0 * with_epilogue / analysis_result.functions.size());
    fmt::print("Complete:        {} ({:.1f}%)\n\n", with_both,
               100.0 * with_both / analysis_result.functions.size());

    // Print first 20 functions
    fmt::print("First 20 Functions:\n");
    fmt::print("---------------------------------------\n");

    for (size_t i = 0; i < std::min(size_t(20), analysis_result.functions.size()); i++) {
        const auto& func = analysis_result.functions[i];
        fmt::print("  {:08X}-{:08X}  {:5} bytes  {}{}\n",
                   func.start_addr,
                   func.end_addr,
                   func.size,
                   func.name,
                   func.has_prologue ? fmt::format(" (frame={})", func.stack_frame_size) : "");
    }


    fmt::print("\n... ({} more functions)\n\n",
               analysis_result.functions.size() - std::min(size_t(20), analysis_result.functions.size()));


    // Control flow analysis
    PSXRecomp::ControlFlowAnalyzer cfg_analyzer(*exe);
    auto all_cfgs = cfg_analyzer.analyze_all_functions(analysis_result.functions);

    // Print CFG summary statistics
    fmt::print("=== Control Flow Summary ===\n\n");

    int total_basic_blocks = 0;
    int total_loops = 0;
    int total_branches = 0;

    for (const auto& [func_addr, cfg] : all_cfgs) {
        total_basic_blocks += static_cast<int>(cfg.blocks.size());
        total_loops += cfg.loop_count;

        // Count branch instructions across all blocks
        for (const auto& [block_addr, block] : cfg.blocks) {
            if (block.exit_instr.type == PSXRecomp::ControlFlowType::Branch) {
                total_branches++;
            }
        }
    }

    fmt::print("Basic Blocks:       {}\n", total_basic_blocks);
    fmt::print("Loops Detected:     {}\n", total_loops);
    fmt::print("Branch Instructions: {}\n", total_branches);
    fmt::print("Avg Blocks/Function: {:.1f}\n\n",
               static_cast<double>(total_basic_blocks) / all_cfgs.size());

    // Show detailed CFG for first function (as an example)
    if (!analysis_result.functions.empty() && !all_cfgs.empty()) {
        const auto& first_func = analysis_result.functions[0];
        const auto& first_cfg = all_cfgs.at(first_func.start_addr);

        fmt::print("Example: CFG for {} (0x{:08X}):\n", first_func.name, first_func.start_addr);
        fmt::print("---------------------------------------\n");
        fmt::print("  Blocks: {}\n", first_cfg.blocks.size());
        fmt::print("  Loops:  {}\n", first_cfg.loop_count);

        if (first_cfg.blocks.size() > 1) {
            fmt::print("\n  Block Details:\n");
            int count = 0;
            for (const auto& block_addr : first_cfg.block_order) {
                const auto& block = first_cfg.blocks.at(block_addr);
                fmt::print("    Block 0x{:08X}: {} instructions", block.start_addr, block.instruction_count);

                if (block.is_entry) fmt::print(" [ENTRY]");
                if (block.is_exit) fmt::print(" [EXIT]");
                if (block.is_loop_header) fmt::print(" [LOOP]");

                fmt::print("\n");
                fmt::print("      Successors: {}", block.successors.size());
                if (!block.successors.empty()) {
                    fmt::print(" (");
                    for (size_t i = 0; i < block.successors.size(); i++) {
                        fmt::print("0x{:08X}", block.successors[i]);
                        if (i + 1 < block.successors.size()) fmt::print(", ");
                    }
                    fmt::print(")");
                }
                fmt::print("\n");

                // Only show first 5 blocks for brevity
                if (++count >= 5 && first_cfg.blocks.size() > 5) {
                    fmt::print("    ... ({} more blocks)\n", first_cfg.blocks.size() - 5);
                    break;
                }
            }
        }
        fmt::print("\n");
    }

    // C code generation
    PSXRecomp::CodeGenConfig codegen_config;
    codegen_config.emit_comments = true;
    codegen_config.emit_line_numbers = true;
    codegen_config.split_mid_function_targets = !overlay_mode;
    codegen_config.overlay_mode = overlay_mode;
    codegen_config.ws_signed_x_bound_sites = ws_signed_x_bound_sites;
    codegen_config.ws_sprite_tag_funcs = ws_tag_funcs;
    codegen_config.data_shard_funcs = ds_funcs;
    codegen_config.mod_function_entry_funcs = mod_entry_funcs;
    codegen_config.hot_funcs = hot_funcs;
    codegen_config.load_charge_batch_funcs = load_charge_batch_funcs;
    codegen_config.vsync_query_hle_funcs = vsync_query_hle_funcs;
    codegen_config.ws_bg2d_init_func = ws_bg2d_init_func;
    codegen_config.ws_cull_bias_sites  = ws_cull_bias;
    codegen_config.ws_cull_range_sites = ws_cull_range;
    codegen_config.ws_cull_a1_sites    = ws_cull_a1;
    codegen_config.ws_cull_screen_x_sites = ws_cull_screen_x;
    codegen_config.ws_cull_activation_guard_pixels =
        ws_cull_activation_guard_pixels;
    codegen_config.ws_cull_slti_sites  = ws_cull_slti;
    codegen_config.ws_cull_slti_lower_sites = ws_cull_slti_lower;
    codegen_config.ws_cull_bltz_sites  = ws_cull_bltz;
    codegen_config.ws_cull_negsub_sites = ws_cull_negsub;
    codegen_config.ws_cull_vxrange_sites = ws_cull_vxrange;
    codegen_config.ws_cull_depth_sites = ws_cull_depth;
    codegen_config.ws_cull_plane_nx_sites = ws_cull_plane_nx;
    codegen_config.ws_cull_xclip_load_sites = ws_cull_xclip_load;
    codegen_config.ws_cull_nclip_keep_sites = ws_cull_nclip_keep;
    codegen_config.ws_cull_branch_keep_sites = ws_cull_branch_keep;
    codegen_config.ws_cull_keep_sites = ws_cull_keep;
    codegen_config.ws_cull_widen_sites = ws_cull_widen;
    codegen_config.ws_cull_angle_sites = ws_cull_angle;
    codegen_config.ws_aspect_cone = ws_aspect_cone;
    codegen_config.ws_cull_w_imms      = ws_cull_w_imms;
    codegen_config.ws_cull_h_imms      = ws_cull_h_imms;
    codegen_config.ws_backdrop_x_sites = ws_backdrop_x;
    codegen_config.ws_backdrop_unsquash_funcs = ws_backdrop_unsquash;
    codegen_config.ws_auto_screen_x_cull = ws_auto_screen_x_cull;
    codegen_config.persist_init_store_sites = persist_init_sites;
    codegen_config.ws_auto_backdrop_preload = ws_auto_backdrop_preload;
    codegen_config.ws_bg2d_count_site    = ws_bg2d_count_site;
    codegen_config.ws_bg2d_startcol_site = ws_bg2d_startcol_site;
    codegen_config.ws_bg2d_startx_site   = ws_bg2d_startx_site;
    codegen_config.ws_bg2d_stream_left_site  = ws_bg2d_stream_left_site;
    codegen_config.ws_bg2d_stream_right_site = ws_bg2d_stream_right_site;
    codegen_config.ws_bg2d_bufbase_site = ws_bg2d_bufbase_site;
    codegen_config.ws_bg2d_cap_site     = ws_bg2d_cap_site;
    if (ws_bg2d_count_site || ws_bg2d_startcol_site || ws_bg2d_startx_site)
        fmt::print("  ws_bg2d 2D-background widen = ON (count=0x{:08X} startcol=0x{:08X} startx=0x{:08X})\n",
                   ws_bg2d_count_site, ws_bg2d_startcol_site, ws_bg2d_startx_site);
    if (ws_auto_screen_x_cull)
        fmt::print("  ws_auto_screen_x_cull = ON (render-funnel FOV widening)\n");
    if (!persist_init_sites.empty())
        fmt::print("  persist_options = {} init-store hook(s)\n", persist_init_sites.size());
    if (ws_auto_backdrop_preload)
        fmt::print("  ws_auto_backdrop_preload = ON (far-backdrop column preload)\n");

    // Load per-game annotations: annotations/<exe_stem>_annotations.csv
    // Silently skipped if the file doesn't exist.
    PSXRecomp::AnnotationTable annotations;
    {
        std::string stem = exe_path.filename().string();
        std::string ann_path = "annotations/" + stem + "_annotations.csv";
        if (annotations.load(ann_path.c_str()))
            fmt::print("✓ Loaded {} annotations from {}\n\n", annotations.count(), ann_path);
        else
            fmt::print("  (No annotations file found at {})\n\n", ann_path);
    }

    PSXRecomp::CodeGenerator codegen(*exe, codegen_config);
    codegen.set_annotations(&annotations);

    // Generate code for first 5 functions (as examples)
    fmt::print("=== C Code Generation Examples ===\n\n");
    fmt::print("Generating code for first 5 functions...\n\n");

    std::vector<PSXRecomp::Function> sample_funcs;
    for (size_t i = 0; i < std::min(size_t(5), analysis_result.functions.size()); i++) {
        sample_funcs.push_back(analysis_result.functions[i]);
    }

    auto generated = codegen.generate_all_functions(sample_funcs, all_cfgs);

    // Print first generated function as example
    if (!generated.empty()) {
        fmt::print("Example Generated Function:\n");
        fmt::print("---------------------------------------\n");
        fmt::print("{}\n", generated[0].full_code);
        fmt::print("---------------------------------------\n\n");
    }

    // Generate full C file and save to the generated directory
    // Use argv[2] as output path if provided, otherwise derive from input filename
    // Use filename() not stem() because ".36" in SCUS_942.36 is part of the serial, not an extension
    std::string exe_stem = exe_path.filename().string();
    std::filesystem::create_directories(out_dir);
    std::filesystem::path output_filename = out_dir / (exe_stem + "_full.c");
    fmt::print("Generating complete C file: {}\n", output_filename.string());

    std::string full_c_code = codegen.generate_file(analysis_result.functions, all_cfgs);
    std::set<uint32_t> dispatch_addrs;
    {
        const std::string marker = "void func_";
        size_t pos = 0;
        while ((pos = full_c_code.find(marker, pos)) != std::string::npos) {
            size_t hex_pos = pos + marker.size();
            if (hex_pos + 8 <= full_c_code.size() &&
                full_c_code.compare(hex_pos + 8, 14, "(CPUState* cpu") == 0) {
                std::string hex = full_c_code.substr(hex_pos, 8);
                dispatch_addrs.insert((uint32_t)std::strtoul(hex.c_str(), nullptr, 16));
            }
            pos = hex_pos + 8;
        }
    }
    // Data/untranslatable entries are emitted as fail-closed
    // psx_unknown_dispatch stubs. Do not put those stubs in the callable game
    // dispatch table: a runtime overlay can legitimately replace the same EXE
    // address, and a tiny stub range (especially a single NOP) can
    // coincidentally byte-match the live overlay continuation. Advertising the
    // stub as native-safe then turns valid dynamic code into a false fail-fast.
    for (const auto& gen_func : codegen.last_gen_funcs()) {
        if (gen_func.dispatchable) continue;
        const std::string prefix = "func_";
        if (gen_func.function_name.rfind(prefix, 0) != 0 ||
            gen_func.function_name.size() < prefix.size() + 8) {
            continue;
        }
        const std::string hex =
            gen_func.function_name.substr(prefix.size(), 8);
        dispatch_addrs.erase(
            (uint32_t)std::strtoul(hex.c_str(), nullptr, 16));
    }
    if (dispatch_addrs.empty() && codegen.last_gen_funcs().empty()) {
        for (const auto& func : analysis_result.functions) {
            dispatch_addrs.insert(func.start_addr);
        }
    }

    // Split-TU output: instead of one monolithic <exe_stem>_full.c, write a
    // shared declarations header plus N shards bucketed by cumulative line
    // count, so the game build can compile the shards in parallel. The
    // in-memory full_c_code / dispatch_addrs scan above is unaffected — only
    // the on-disk representation changes. Function bodies are byte-identical
    // to what full_c_code would have contained; see
    // CodeGenerator::last_gen_funcs() / build_shared_decls_header().
    if (overlay_mode) {
        // Overlays are single small TUs consumed by compile_overlays.py, which
        // reads <stem>_full.c directly. Emit the monolith; the split path below
        // would delete _full.c and leave only shards, breaking overlay compile
        // (no_output). Splitting a small overlay TU has no parallel-compile
        // benefit anyway.
        std::filesystem::path full_path(output_filename);
        if (write_file_if_changed(full_path, full_c_code)) {
            fmt::print("✓ Saved overlay monolith to {}\n", output_filename.string());
        } else {
            fmt::print("✓ Overlay monolith unchanged ({})\n", output_filename.string());
        }
    } else {
        // 1. Shared declarations header (write-if-changed preserves Ninja mtimes).
        std::filesystem::path decls_filename = out_dir / (exe_stem + "_decls.h");
        {
            const std::string decls =
                codegen.build_shared_decls_header(codegen.last_gen_funcs());
            if (write_file_if_changed(decls_filename, decls)) {
                fmt::print("✓ Saved shared decls header to {}\n", decls_filename.string());
            } else {
                fmt::print("✓ Shared decls header unchanged ({})\n", decls_filename.string());
            }
        }

        // 2. Bucket generated functions into shards by cumulative line count.
        const int SHARD_LINE_BUDGET = 40000;
        const std::vector<PSXRecomp::GeneratedFunction>& gen_funcs_for_shards = codegen.last_gen_funcs();
        std::string decls_basename = exe_stem + "_decls.h";

        int shard_index = 0;
        int shard_lines = 0;
        int total_shard_lines = 0;
        int shards_written = 0;
        std::stringstream shard_buf;
        auto flush_shard = [&]() {
            if (shard_buf.tellp() == std::streampos(0)) return;
            std::filesystem::path shard_filename =
                out_dir / fmt::format("{}_full_{:02d}.c", exe_stem, shard_index);
            std::string body = fmt::format(
                "/* Generated by PSXRecomp - full.c shard {}. DO NOT EDIT. */\n"
                "#include \"{}\"\n\n{}",
                shard_index, decls_basename, shard_buf.str());
            if (write_file_if_changed(shard_filename, body)) {
                fmt::print("✓ Saved shard {} ({} lines) to {}\n",
                           shard_index, shard_lines, shard_filename.string());
                ++shards_written;
            } else {
                fmt::print("✓ Shard {} unchanged ({} lines)\n", shard_index, shard_lines);
            }
            total_shard_lines += shard_lines;
            shard_index++;
            shard_lines = 0;
            shard_buf.str(std::string());
            shard_buf.clear();
        };

        for (const auto& gf : gen_funcs_for_shards) {
            if (shard_lines > 0 && shard_lines + gf.line_count > SHARD_LINE_BUDGET) {
                flush_shard();
            }
            shard_buf << gf.full_code << "\n";
            shard_lines += gf.line_count;
        }
        flush_shard();

        // 3. Drop orphan shards / monolith after emit (shrink must not leave stale TUs).
        {
            std::error_code rm_ec;
            std::filesystem::remove(output_filename, rm_ec);
            for (int stale = shard_index; stale < 256; stale++) {
                std::filesystem::path stale_shard =
                    out_dir / fmt::format("{}_full_{:02d}.c", exe_stem, stale);
                std::filesystem::remove(stale_shard, rm_ec);
            }
        }

        fmt::print("✓ Wrote {} shards ({} total lines, {} updated) for {}\n\n",
                   shard_index, total_shard_lines, shards_written, exe_stem);
    }

    // Per-function code-range manifest (design §8): consumed by the overlay
    // loader's per-entry validity hash. Emitted alongside _full.c for every
    // build; only the overlay path actually loads it.
    const std::string ranges_manifest = codegen.last_ranges_manifest().empty()
        ? codegen.generate_ranges_manifest(analysis_result.functions, all_cfgs)
        : codegen.last_ranges_manifest();
    {
        std::filesystem::path ranges_filename = out_dir / (exe_stem + "_full.ranges");
        if (write_file_if_changed(ranges_filename, ranges_manifest)) {
            fmt::print("✓ Saved code-range manifest to {}\n\n",
                      ranges_filename.string());
        } else {
            fmt::print("✓ Code-range manifest unchanged ({})\n\n",
                      ranges_filename.string());
        }
    }

    // Generate dispatch table (tomba_dispatch.c)
    // This maps PS1 addresses to compiled C functions so call_by_address() can
    // dispatch dynamic jalr/jr calls to the right compiled function.
    {
        std::filesystem::path dispatch_filename = out_dir / (exe_stem + "_dispatch.c");
        fmt::print("Generating dispatch table: {}\n", dispatch_filename.string());

        std::ostringstream ds;
        ds << "/* Generated by PSXRecomp - dynamic dispatch table */\n";
        ds << "#include \"psx_runtime.h\"\n\n";
        ds << "extern void psx_check_interrupts_dispatch_entry(CPUState* cpu, uint32_t resume_pc);\n\n";
        ds << "extern int dirty_ram_text_native_ok_ranges_from(const uint32_t* lo_len_pairs, uint32_t count, uint32_t exec_pc);\n\n";
        ds << "extern int dirty_ram_text_native_ok_ranges(const uint32_t* lo_len_pairs, uint32_t count);\n\n";

        // Forward declarations
        ds << "/* Forward declarations for all recompiled functions */\n";
        for (uint32_t addr : dispatch_addrs) {
            ds << fmt::format("extern void func_{:08X}(CPUState* cpu);\n", addr);
        }
        ds << "\n";

        // Dispatch function
        uint32_t game_text_start = exe->load_address() & 0x1FFFFFFFu;
        uint32_t game_text_end = game_text_start + exe->code_size();
        ds << "int psx_game_address_in_text(uint32_t addr) {\n";
        ds << "    extern uint32_t psx_ram_canon_code_addr(uint32_t);\n";
        ds << "    uint32_t phys = psx_ram_canon_code_addr(addr) & 0x1FFFFFFFu;\n";
        ds << fmt::format("    return phys >= 0x{:08X}u && phys < 0x{:08X}u;\n",
                          game_text_start, game_text_end);
        ds << "}\n\n";

        // Materialize one sorted data table for dispatch and entry probes. A
        // giant sparse switch is catastrophically slow in Debug/-O0 builds:
        // GCC lowers X4's roughly 60K cases to a multi-megabyte linear compare
        // chain. Binary search stays O(log N) at every optimizer level and
        // avoids emitting the same case set twice.
        struct DispatchRecord {
            uint32_t addr;
            uint32_t resume;
            uint32_t owner;
            uint32_t range_index;
            uint32_t range_count;
        };
        std::vector<DispatchRecord> records;
        records.reserve(dispatch_addrs.size() +
                        (codegen.cps_enabled() ? codegen.cps_continuations().size() : 0));
        for (uint32_t addr : dispatch_addrs)
            records.push_back({addr, 0, addr, 0, 0});
        if (codegen.cps_enabled()) {
            const auto& conts = codegen.cps_continuations();
            for (const auto& [cont, owner] : conts) {
                if (dispatch_addrs.count(cont)) continue;
                // A continuation owned by a fail-closed stub is no more
                // executable than the stub entry itself.
                if (!dispatch_addrs.count(owner)) continue;
                records.push_back({cont, cont, owner, 0, 0});
            }
        }
        // Sort by PHYSICAL address. psx_game_find_entry() compares masked
        // addresses so a KUSEG-executing guest still matches KSEG-normalized
        // keys; the search invariant must therefore be the masked order too.
        // Within one segment this is identical to sorting by the raw address.
        std::sort(records.begin(), records.end(),
                  [](const DispatchRecord& a, const DispatchRecord& b) {
                      return (a.addr & 0x1FFFFFFFu) < (b.addr & 0x1FFFFFFFu);
                  });

        // Attach the exact CFG instruction ranges from the manifest to every
        // dispatch owner. Mid-function alias wrappers have no separate F record,
        // so resolve them to the containing host range. Mutable data and
        // jump-table gaps sharing a page remain deliberately excluded.
        using CodeRange = std::pair<uint32_t, uint32_t>;  // virtual lo, byte len
        std::map<uint32_t, std::vector<CodeRange>> function_ranges;
        {
            std::istringstream input(ranges_manifest);
            std::string line;
            uint32_t current_owner = 0;
            while (std::getline(input, line)) {
                std::istringstream fields(line);
                char kind = 0;
                fields >> kind;
                if (kind == 'F') {
                    fields >> std::hex >> current_owner;
                    function_ranges[current_owner];
                } else if (kind == 'R' && current_owner != 0) {
                    uint32_t lo = 0, len = 0;
                    fields >> std::hex >> lo >> len;
                    if (len != 0)
                        function_ranges[current_owner].push_back({lo, len});
                }
            }
        }
        std::vector<CodeRange> flat_ranges;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> owner_spans;
        auto resolve_owner_ranges = [&](uint32_t owner)
            -> const std::vector<CodeRange>* {
            auto exact = function_ranges.find(owner);
            if (exact != function_ranges.end() && !exact->second.empty())
                return &exact->second;
            for (const auto& [entry, ranges] : function_ranges) {
                (void)entry;
                for (const auto& [lo, len] : ranges) {
                    const uint64_t hi = (uint64_t)lo + len;
                    if (owner >= lo && (uint64_t)owner < hi) return &ranges;
                }
            }
            return nullptr;
        };
        for (auto& rec : records) {
            auto cached = owner_spans.find(rec.owner);
            if (cached == owner_spans.end()) {
                const uint32_t first = (uint32_t)flat_ranges.size();
                const auto* ranges = resolve_owner_ranges(rec.owner);
                if (ranges)
                    flat_ranges.insert(flat_ranges.end(), ranges->begin(), ranges->end());
                const uint32_t count = (uint32_t)flat_ranges.size() - first;
                cached = owner_spans.emplace(rec.owner,
                                             std::make_pair(first, count)).first;
            }
            rec.range_index = cached->second.first;
            rec.range_count = cached->second.second;
        }

        ds << "typedef struct { uint32_t lo; uint32_t len; } PsxGameCodeRange;\n";
        ds << "static const PsxGameCodeRange k_psx_game_code_ranges[] = {\n";
        if (flat_ranges.empty()) ds << "    {0u, 0u},\n";
        for (const auto& [lo, len] : flat_ranges)
            ds << fmt::format("    {{0x{:08X}u, 0x{:X}u}},\n", lo, len);
        ds << "};\n\n";

        ds << "typedef void (*PsxGameDispatchFn)(CPUState* cpu);\n";
        ds << "typedef struct {\n";
        ds << "    uint32_t addr;\n";
        ds << "    uint32_t resume_pc;\n";
        ds << "    uint32_t range_index;\n";
        ds << "    uint32_t range_count;\n";
        ds << "    PsxGameDispatchFn fn;\n";
        ds << "} PsxGameDispatchEntry;\n\n";
        ds << "static const PsxGameDispatchEntry k_psx_game_dispatch[] = {\n";
        for (const auto& rec : records) {
            ds << fmt::format("    {{0x{:08X}u, 0x{:08X}u, {}u, {}u, func_{:08X}}},\n",
                              rec.addr, rec.resume, rec.range_index,
                              rec.range_count, rec.owner);
        }
        ds << "};\n";
        ds << fmt::format("#define PSX_GAME_DISPATCH_COUNT {}u\n\n", records.size());
        ds << "/* PS1 segments alias the same physical RAM. A game whose PS-X EXE\n";
        ds << " * header carries KUSEG addresses (load address and entry PC without the\n";
        ds << " * KSEG bit) executes with a KUSEG PC, while this table is keyed by the\n";
        ds << " * recompiler's KSEG-normalized addresses. Comparing raw values made every\n";
        ds << " * lookup fail for such a title: 0x0001xxxx is always below 0x8001xxxx, so\n";
        ds << " * the search collapsed and returned no entry, silently routing all game\n";
        ds << " * code to the interpreter. Compare the 29-bit physical address instead;\n";
        ds << " * the table is sorted by the same masked key. */\n";
        ds << "static const PsxGameDispatchEntry* psx_game_find_entry(uint32_t addr) {\n";
        ds << "    extern uint32_t psx_ram_canon_code_addr(uint32_t);\n";
        ds << "    addr = psx_ram_canon_code_addr(addr);\n";
        ds << "    const uint32_t want = addr & 0x1FFFFFFFu;\n";
        ds << "    uint32_t lo = 0, hi = PSX_GAME_DISPATCH_COUNT;\n";
        ds << "    while (lo < hi) {\n";
        ds << "        uint32_t mid = lo + (hi - lo) / 2;\n";
        ds << "        uint32_t key = k_psx_game_dispatch[mid].addr & 0x1FFFFFFFu;\n";
        ds << "        if (want < key) hi = mid;\n";
        ds << "        else if (want > key) lo = mid + 1;\n";
        ds << "        else return &k_psx_game_dispatch[mid];\n";
        ds << "    }\n";
        ds << "    return 0;\n";
        ds << "}\n\n";

        ds << "/* Exact static-code validity for this entry's emitted CFG ranges. */\n";
        ds << "int psx_game_text_native_ok(uint32_t addr) {\n";
        ds << "    const PsxGameDispatchEntry* entry = psx_game_find_entry(addr);\n";
        ds << "    if (!entry || entry->range_count == 0) return 0;\n";
        ds << "    return dirty_ram_text_native_ok_ranges_from(\n";
        ds << "        &k_psx_game_code_ranges[entry->range_index].lo, entry->range_count, addr);\n";
        ds << "}\n\n";

        ds << "/* Full-range validity for straight-line interpreter-to-AOT handoff. */\n";
        ds << "int psx_game_text_native_ok_full(uint32_t addr) {\n";
        ds << "    const PsxGameDispatchEntry* entry = psx_game_find_entry(addr);\n";
        ds << "    if (!entry || entry->range_count == 0) return 0;\n";
        ds << "    return dirty_ram_text_native_ok_ranges(\n";
        ds << "        &k_psx_game_code_ranges[entry->range_index].lo, entry->range_count);\n";
        ds << "}\n\n";

        ds << "/* Maps PS1 address to compiled game code. Returns 1 if dispatched, 0 if unknown. */\n";
        ds << "int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr) {\n";
        ds << "    const PsxGameDispatchEntry* entry = psx_game_find_entry(addr);\n";
        ds << "    if (!entry) return 0;\n";
        ds << "    if (entry->range_count == 0 || !dirty_ram_text_native_ok_ranges_from(\n";
        ds << "            &k_psx_game_code_ranges[entry->range_index].lo, entry->range_count, addr)) return 0;\n";
        ds << "    psx_check_interrupts_dispatch_entry(cpu, addr);\n";
        if (codegen.cps_enabled())
            ds << "    cpu->pc = entry->resume_pc;\n";
        ds << "    /* load_accel: cycle-faithful VSync(-1) query (config-gated). */\n";
        ds << "    {\n";
        ds << "        extern int psx_vsync_query_hle_try(CPUState* cpu, uint32_t addr);\n";
        ds << "        if (psx_vsync_query_hle_try(cpu, addr)) return 1;\n";
        ds << "    }\n";
        ds << "    entry->fn(cpu);\n";
        ds << "    return 1;\n";
        ds << "}\n";

        // Non-destructive companion to psx_dispatch_game_compiled: is `addr` a PC
        // the top-level dispatch can re-enter (a compiled function entry, or a CPS
        // continuation resume-point)? The precise event slicer (dirty_ram_interp.c)
        // needs this to know when it may hand a resume PC back to the dispatcher:
        // a MID-function clean-text PC is NOT re-enterable (the switch falls to
        // `default: return 0` and the dirty path returns 0 -> top-level PC=0 exit),
        // so the slicer must keep interpreting until cpu->pc lands on a true entry.
        // Same precise address set as dispatch, without executing the entry.
        ds << "\n/* 1 iff addr is a re-enterable compiled entry/continuation (no exec). */\n";
        ds << "int psx_game_is_function_entry(uint32_t addr) {\n";
        ds << "    return psx_game_find_entry(addr) != 0;\n";
        ds << "}\n";

        if (codegen.cps_enabled()) {
            // RECURSION_BUG.md §25 — mark CPS mode at startup so runtime code that
            // routes CPS continuations (overlay_loader.c) sees the contract.
            // Static ctor: no clash with the BIOS dispatch's marker.
            ds << "\n/* CPS runtime-mode marker (the overlay loader reads g_psx_cps_mode). */\n";
            ds << "static void psx_cps_mark_game(void) {\n";
            ds << "    extern int g_psx_cps_mode; g_psx_cps_mode = 1;\n";
            ds << "}\n";
            // Run psx_cps_mark_game before main(). __attribute__((constructor)) is
            // GCC/Clang-only; MSVC uses a static initializer pointer in .CRT$XCU.
            ds << "#if defined(_MSC_VER)\n";
            ds << "#pragma section(\".CRT$XCU\", read)\n";
            ds << "__declspec(allocate(\".CRT$XCU\")) static void (*psx_cps_mark_game_ctor)(void) = psx_cps_mark_game;\n";
            ds << "#else\n";
            ds << "__attribute__((constructor)) static void psx_cps_mark_game_ctor(void) { psx_cps_mark_game(); }\n";
            ds << "#endif\n";
        }

        if (write_file_if_changed(dispatch_filename, ds.str())) {
            fmt::print("✓ Dispatch table written ({} entries)\n\n",
                       dispatch_addrs.size());
        } else {
            fmt::print("✓ Dispatch table unchanged ({} entries)\n\n",
                       dispatch_addrs.size());
        }
    }

    return 0;
}
