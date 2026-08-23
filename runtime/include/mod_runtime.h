#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>
#include <vector>
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace PSXRecompV4 {

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path = {},
                            std::string* error = nullptr);
bool mod_runtime_commit(const std::filesystem::path& disc_path = {},
                        std::string* error = nullptr);
bool mod_runtime_commit(const std::filesystem::path& disc_path,
                        std::string* error,
                        bool persist);
/* Drop the in-session mod plan for a vanilla netplay launch without rewriting
 * the user's persisted offline selection on disk. */
bool mod_runtime_clear_for_netplay(std::string* error = nullptr);
/* Online lobby: apply the host match_caps mod plan (no disk persist).
 * LAN / empty plan: clear for a vanilla session. */
bool mod_runtime_commit_for_netplay(const std::filesystem::path& disc_path = {},
                                    std::string* error = nullptr);

/* ===== PSX-Link pair mod propagation ====================================
 * A link machine runs the netplay CLIENT plus a spawned FOLLOWER that never
 * joins the lobby, so the follower cannot read match_caps and used to clear
 * to vanilla — the client would boot 8 MB + patched disc while its own
 * follower ran stock (a RAM-part digest fork at tick 0). The driver
 * serializes the session's applied plan and hands it to the follower in the
 * environment; the follower applies THAT, so both consoles on the machine
 * share one plan by construction.
 *
 * Spec grammar (compact, env-safe, order-stable):
 *   spec  := entry (';' entry)*
 *   entry := id '@' ver [':' feat (',' feat)*]
 * An empty/absent spec means vanilla. */
std::string mod_runtime_link_spec_from_session();
/* Transport-agnostic session plan. The WS lobby carries the host plan in
 * match_caps; PSX-Link carries it in the follower's environment; LAN /
 * Direct-IP has no server to relay anything, so its host publishes this same
 * spec in its session datagrams and every peer stores it here. When set, it
 * is what commit_for_netplay applies (a WS lobby still wins). Empty = the
 * session is vanilla. */
void mod_runtime_set_session_plan_spec(const std::string& spec);
const std::string& mod_runtime_session_plan_spec();
/* The HOST's portable plan fingerprint for this session, when the transport
 * published one. After applying the plan, a peer compares its own; a
 * mismatch means the two catalogs resolve the same plan differently (option
 * defaults, a repackaged mod) and the launch is refused rather than desyncing
 * on the first tick. Empty = nothing to check. */
void mod_runtime_set_session_plan_fp(const std::string& fp);
const std::string& mod_runtime_session_plan_fp();
bool mod_runtime_verify_session_plan_fp(std::string* error = nullptr);
/* Apply a spec produced by the above (no disk persist). Empty spec clears. */
bool mod_runtime_apply_link_spec(const std::string& spec,
                                 const std::filesystem::path& disc_path = {},
                                 std::string* error = nullptr);
/* Resolution fingerprint of the CURRENT selection, computed without the
 * local disc identity so two peers with the same catalog + same plan produce
 * the same string. Covers package versions, feature enables, EVERY option
 * value and the resolved write/overlay set — i.e. everything the plan spec
 * cannot express by itself. Resolve-only: no disc hashing, no derived-image
 * materialization, safe to call from the lobby. */
std::string mod_runtime_plan_fingerprint_portable();
/* Stable 32-bit digest of a spec — pair-cfg cross-check so a driver/follower
 * plan disagreement refuses to pair instead of desyncing at tick 0. */
uint32_t mod_runtime_link_spec_hash(const std::string& spec);
bool mod_runtime_export_package(const std::string& id, const std::string& version,
                                std::vector<uint8_t>& out, std::string* sha256_hex,
                                std::string* error = nullptr);
bool mod_runtime_install_bytes(const uint8_t* data, size_t size,
                               std::string* error = nullptr);
const std::string& mod_runtime_fingerprint();
const std::filesystem::path& mod_runtime_effective_disc_path();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

} // namespace PSXRecompV4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called before a guest dispatch. Applies the complete main-EXE plan
 * transactionally on the first dispatch to the configured entry point. */
void mod_runtime_on_dispatch(uint32_t target);
/* A full-machine savestate restores guest RAM after the initial entry-point
 * application. Reapply the already-validated main-EXE plan so the current
 * enabled mod selection remains authoritative after the restore. */
void mod_runtime_on_savestate_loaded(void);
/* Invokes activation callbacks for the committed plan. Call after the final
 * launcher commit and before renderer/window initialization. */
void mod_runtime_activate_plugins(void);
void mod_runtime_on_vblank(void);
void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                   uint8_t* bytes, uint32_t size);
void mod_runtime_enable_disc_patches(void);

#ifdef __cplusplus
}
#endif
