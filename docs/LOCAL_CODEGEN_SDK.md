# Local codegen SDK

Headless contract for regenerating an existing psxrecomp game project from a
user-supplied disc. Intended for `recomp-ui` setup flows and RetComM launcher
automation. This does **not** redistribute disc images.

PGO (optional) runs **only on the user’s machine** during local rebuild when
`game.toml` has `[pgo] enabled = true`. CI must not set `PSX_PGO`.

## Setup host (CI without game/BIOS generated C)

Games can ship a **setup host**: `psx-runtime` linked **without** game C and
**without** BIOS backends (`-DPSXRECOMP_FORCE_SETUP_HOST=ON`, or legacy
`-DBPE_FORCE_SETUP_HOST=ON`) via `psxrecomp_add_game_runtime(...)`, and with
`PSX_SETUP_WIZARD=ON` / `ENABLE_SETUP_WIZARD` so first-run Generate & rebuild
actually appears. CI never
needs BIOS dumps or private assets. First-run Generate emits OpenBIOS (from
bundled `openbios.bin`) and optional SCPH1001 (player dump), then game C, then
rebuild links everything into `build-release/` (or the title’s
`build_dir_name`).

**Layout after Generate & rebuild:** the zip-root exe stays the setup host;
the playable product (exe + `bios/` + `mods/` + `assets/` + `settings.toml`)
lives under `build-release/`. Reopening the zip-root exe **forwards** to that
product binary (`psxrecomp_codegen_host_forward_if_built`). Opt out with
`PSXRECOMP_NO_FORWARD=1` or the title’s `*_FORCE_SETUP=1`.

| Piece | Role |
|-------|------|
| Setup exe (zip root) | First-run wizard; after rebuild, forwards to `build-release/` |
| Product exe | `build-release/<exe>` — Play, bios/mods/assets/settings |
| Game zip `psxrecomp/` | submodule tree: CLI, tools, emitters, OpenBIOS profiles |
| `psxrecomp_cli.py` | Generate / rebuild / verify-disc (in the submodule) |
| `tools/package_setup_host.sh` | Universal setup-host zip packager |
| `tools/ci/*.sh` | normalize version, clear generated, record pins, build emitters |
| `tools/fetch_toolchain.sh` | Optional: download/unpack `cmake-clang-v1` (CI embed or local) |
| `tools/toolchain_pack.py` | CLI: resolve / download / unpack into `toolchain/` + shared cache |
| `tools/stage_setup_sdk.sh` | Pack: emitters, OpenBIOS checks, optional `toolchain/`, MinGW DLLs |
| `tools/bundle_mingw_dlls.sh` | Windows: copy MinGW runtime DLLs next to host + emitters |
| Project `toolchain/` | Stamp file `.psxrecomp-bin` pointing at the shared pack `bin/` |
| Shared toolchain cache | `%LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/` (Windows) or `~/.local/share/retcomm/toolchains/cmake-clang-v1/` — same tree RetComM uses |
| `docs/ci/` | Composite actions + [`templates/setup-release.yml`](ci/templates/setup-release.yml) |
| `docs/GAME_PROJECT_SETUP.md` | Submodules, CI template usage, bundled-release checklist |
| Game sources | `game.toml`, seeds, `CMakeLists.txt` at repo root; `psxrecomp/`, `recomp-ui/` submodules |

RetComM harvests emitters into the SDK cache and **downloads** `cmake-clang-v1`
(no separate tools zip; game zips stay lean). The setup host and CLI install the
portable pack into the **shared RetComM cache**
(`%LOCALAPPDATA%/retcomm/toolchains/cmake-clang-v1/<tag>/`, or XDG equivalent)
with host-native `curl` + `tar`/`unzip` when possible — so Microsoft Store
Python AppData redirection cannot hide cmake. Offline zips unpack to the same
path. A legacy `%LOCALAPPDATA%/psxrecomp/…` cache is migrated automatically.
Override with `RETCOMM_TOOLCHAIN_DIR`. Require a floor version via
`RETCOMM_TOOLCHAIN_MIN_VERSION` / `ensure-toolchain --min-version` (optional;
default is none — wizard/`ensure-toolchain` fetch GitHub `/releases/latest`).
Generate/rebuild still need Python 3
for `psxrecomp_cli.py`.

## Commands

```bash
python psxrecomp/psxrecomp_cli.py verify-disc \
  --config game.toml --disc path/to/dump.bin [--json-progress]

python psxrecomp/psxrecomp_cli.py generate \
  --config game.toml --project-root . --disc path/to/dump.iso \
  [--bios path/to/SCPH1001.BIN] [--force-bios] \
  [--skip-hash-check] [--force-prepare] [--json-progress]

python psxrecomp/psxrecomp_cli.py ensure-toolchain \
  [--project-root .] [--from-zip cmake-clang-v1-linux-x64.zip] [--no-download]

python psxrecomp/psxrecomp_cli.py rebuild \
  --config game.toml --project-root . \
  --build-dir build-release --target psx-runtime \
  --exe-basename Bomberman_Party_Edition_Recompiled \
  [--disc path/to/game.cue] [--toolchain-zip path/to/pack.zip] \
  [--no-toolchain-download] [--no-pgo] [--force-pgo] [--json-progress]

python psxrecomp/psxrecomp_cli.py pgo-train \
  --config game.toml --build-dir build-release \
  --exe-basename Bomberman_Party_Edition_Recompiled \
  [--disc …] [--train-secs 120] [--train-runs 3] [--json-progress]
```

`generate` normalizes the dump via `tools/prepare_disc.py` when needed, then
runs `psxrecomp-game --config game.toml` into `[recompiler] out_dir`.

`rebuild` runs cmake. If `[pgo] enabled = true` (and `--no-pgo` not set), it
instruments, trains (`scenario` / timed boot), then rebuilds with profiles.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | success |
| 1 | runtime / generation / build failure |
| 2 | usage / argument error |
| 3 | disc verification failure |

## JSONL progress (`--json-progress`)

Stdout is reserved for one JSON object per line. Useful events:

| `event` | Notes |
|---------|--------|
| `phase` | `verify`, `prepare_disc`, `emit`, `build`, `pgo_*`, `done` |
| `disc` | digests after verification |
| `log` | mirrored tool chatter |
| `result` | final payload |
| `error` | `message`, `code` |

## Portable recomp-ui host (`psxrecomp/host/`)

Prefer `psxrecomp_add_game_runtime(...)` — it compiles
`psxrecomp/host/psxrecomp_codegen_host.c` and sets `PSX_HAS_GAME_CODEGEN`.

Titles keep a thin root `codegen_setup.c` that fills
`PsxrecompCodegenHostConfig` and exposes apply/relaunch hooks:

```c
psxrecomp_codegen_host_apply(&gi, &my_cfg);

/* after recomp_launcher_run_window: */
if (lr == RECOMP_LAUNCHER_RESULT_RELAUNCH)
    psxrecomp_codegen_host_relaunch_or_exit(disc_path);
```

### Modular `[pgo]` (game.toml) — opt-in

PGO runs only when the title sets `enabled = true`. Framework defaults when
enabled: **60s × 2 runs**, `mute_host_audio = true`, `hide_video = true`.
Games may lengthen trains in their own `game.toml`.

```toml
[pgo]
enabled = true                 # required to opt in
train_secs = 60                # optional override
train_runs = 2                 # optional override
mute_host_audio = true         # default: discard SDL output (SPU still runs)
hide_video = true              # default: --headless (no on-screen FMV)
scenario = "boot_timed"        # title-authored workload hint
```

Omit the section or set `enabled = false` to skip. During train the CLI/UI show
a **WARNING** — do not cancel the process (or close a visible window if
`hide_video = false`).

`hide_video` keeps guest MDEC/FMV decode in the profile but shows nothing on
screen (avoids abrasive/flickering FMV). Use `--pgo-video` only when you need
host present paths in the profile.

### Env overrides

| Env | Role |
|-----|------|
| `PSXRECOMP_PROJECT_ROOT` / game-specific | project root |
| `PSXRECOMP_BUILD_DIR` / game-specific | cmake build dir |
| `PSXRECOMP_FORCE_SETUP` / game-specific | force setup wizard |

Project-root discovery order: env → walk **cwd** → walk **exe dir**
(`$APPIMAGE` parent or `/proc/self/exe` / `GetModuleFileName`). GUI launches
that leave cwd as `$HOME` still find a setup zip next to the binary.
| `PSXRECOMP_GAME` | path to `psxrecomp-game` binary |
| `PSX_HOST_MUTE=1` | discard host SDL audio (SPU still runs; set by PGO train) |
| `PSX_HEADLESS=1` | no SDL window (set by PGO train when `hide_video`) |
| `SDL_VIDEODRIVER=dummy` | set by default with headless train |
| `PYTHON` / `CMAKE` | tool overrides |

### Toolchain update prompt (wizard)

The first-run / setup wizard compares the local cmake-clang pack version against
GitHub `/releases/latest` and, when newer, prompts **Update** or **Skip for now**.

- `ensure_toolchain_with_progress(..., download == 2)` forces a re-download from
  latest (update path); `download == 1` fetches only if missing; `0` is cache-only.
- Set `RETCOMM_TOOLCHAIN_SKIP_UPDATE=1` to disable the remote newer-than-local check.

### Broken toolchain heal (wizard open)

`toolchain_is_ready` does not stop at `cmake --version`. It also smoke-tests
`clang` + `ld.lld` (tiny link). If that fails (missing `libicuuc.so.*`, bad
`latest/` pointer, etc.), the host removes the broken `latest/` cache entry,
clears `toolchain/.psxrecomp-bin`, and re-opens wizard page 0 with a repair note
so the player can redownload — without deleting the cache by hand.
