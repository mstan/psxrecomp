# DuckStation oracle (dynamic comparison harness)

PSXRecomp v4 uses a patched build of [stenzek/duckstation](https://github.com/stenzek/duckstation) as a live oracle for the recompiled BIOS. Our runtime speaks JSON-over-TCP on port **4370**; the patched DuckStation speaks the same protocol on **4371**, so tools like `debug_client.py compare` can diff live state between the two at any moment.

## Layout

- **`duckstation/`** (git submodule) — pristine upstream DuckStation source tree, pinned to commit `ffb33c281d196eb8ee0f559085ca285de7cdd51b` (release-20260328 era). Never edited directly.
- **`tools/duckstation/psxrecomp_oracle.patch`** — our changes as a unified diff against the pinned upstream base. Touches 7 files (~1200 lines). Adds `src/core/psxrecomp_debug_server.{cpp,h}`, wires `PSXRecompDebug::Initialize(4371)` into `System::Initialize`, exposes three GPU debug accessors, registers a log channel.
- **`tools/duckstation/setup.sh`** — idempotent. Initializes submodule, fetches + verifies + extracts prebuilt Windows deps (SDL3, Qt6, ffmpeg, …), normalizes the absolute paths baked into the prebuilt CMake metadata, applies the oracle patch.
- **`tools/duckstation/build.sh`** — runs CMake (Visual Studio 17 2022, x64, Release) and MSBuild on `duckstation-qt`. Requires the Visual Studio 2022 "Desktop development with C++" workload (CMake + MSBuild come with it).
- **`tools/fix_duckstation_deps_paths.py`** — helper used by `setup.sh` to rewrite stale `_IMPORT_PREFIX` values in the extracted prebuilt deps.

## First-time setup — Linux / macOS

Use `tools/duckstation_oracle.py`. It installs **outside any game repo**, into
the shared RetComM data root, so one build serves every title:

```
~/.local/share/retcomm/oracle/duckstation/     ($RETCOMM_DATA_DIR, or $RETCOMM_ORACLE_DIR)
  src/     pinned upstream checkout with the oracle patch applied
  build/   cmake/ninja tree
  app/     the portable install that actually gets run (~244 MB, self-contained)
  oracle.json
```

```bash
P=psxrecomp/tools
python3 $P/duckstation_oracle.py doctor          # can this machine build it?
python3 $P/duckstation_oracle.py all             # fetch + patch + build + install
python3 $P/duckstation_oracle.py start --disc disc/game.cue
python3 $P/duckstation_oracle.py status
python3 $P/duckstation_oracle.py stop
```

`gpu_parity.py --start-oracle --disc <cue>` will do the `start` step for you
when nothing is answering on 4371.

### Arch, CachyOS, NixOS: the build runs in a container

DuckStation's CMake **refuses to configure** on Arch-family and NixOS hosts
(`CMakeModules/DuckStationBuildSummary.cmake` → `FATAL_ERROR "Unsupported
environment."`), and the note above that check states you do not have permission
to distribute patches modifying its build system. Its build scripts are
CC-BY-NC-ND-4.0.

So the tool does not patch the check out. It detects the refusal up front and
builds in an environment upstream *does* support: `ubuntu:22.04` via
podman/docker — the exact image its own CI uses
(`.github/workflows/linux-appimage-build.yml`) — installing packages with
upstream's own `scripts/appimage/install-packages.sh`, copied verbatim out of
the pinned checkout. Ubuntu 22.04's glibc is older than any modern host's, so
the binary produced there runs natively on the host afterwards; that is the same
property upstream relies on to ship one AppImage for every distro.

On a distro upstream accepts, the build runs directly on the host. Force either
way with `build --container` / `build --no-container`.

### Notes that cost an afternoon to find

* **The binary's RUNPATH points at the build environment**, and Qt needs its
  plugin tree. `install` copies `dep/prebuilt/<platform>/{lib,plugins}` next to
  the binary and writes a `run-oracle` launcher that sets `LD_LIBRARY_PATH` and
  `QT_PLUGIN_PATH`, then verifies with `ldd` that nothing is unresolved.
* **An unofficial build shows a modal warning dialog before the core thread
  starts.** Under `-nogui` it is invisible, so the process sits at one thread,
  zero CPU, blocked in `ppoll`, with no port and no error. The fix is the key
  the dialog itself writes — `[UI] UnofficialBuildWarningConfirmed = true` —
  which `install` sets. It is a setting, not a patch. (Personal builds are
  explicitly fine under that licence; redistributing one is not, which is why
  this install stays on the machine that built it.)
* **`install` seeds DuckStation's own default `settings.ini`** by running it
  briefly, then merges only the handful of keys an oracle needs. The emulator
  owns that schema; hand-writing the whole file means guessing at it.
* **Wayland support is off by default** (`-DENABLE_WAYLAND=OFF`). It is the only
  dependency the prebuilt bundle does not cover (needs `extra-cmake-modules`),
  and an offscreen oracle never opens a Wayland surface. `build --wayland`
  re-enables it.
* **The oracle server only starts when a system boots.** The patch calls
  `PSXRecompDebug::Initialize(4371)` from `System::BootSystem`, so an idle
  DuckStation with nothing loaded will never open the port.

### The pin

`tools/duckstation/pin.json` is the single source of truth for the upstream
base. Both this tool and the Windows `setup.sh` read it, so the two platforms
cannot drift onto different trees — the oracle patch only applies to that one.
The prebuilt-dependency version and its SHA-256 are **not** pinned here; they
are read from the checkout's own `dep/PREBUILT-VERSION` and
`dep/PREBUILT-SHA256SUMS`, so bumping the base cannot silently pair a new source
tree with an old dependency set.

## First-time setup — Windows

```bash
cd <psxrecomp-v4>
bash tools/duckstation/setup.sh     # ~5 min: clone submodule + fetch/extract deps + apply patch
bash tools/duckstation/build.sh     # ~15-30 min: compile duckstation-qt Release x64
```

The resulting binary is at `duckstation/build/bin/duckstation-qt.exe`. Launch for headless oracle use:

```bash
./duckstation/build/bin/duckstation-qt.exe -bios -nogui -fastboot &
# Wait ~5s for BIOS boot, then:
echo '{"cmd":"ping"}' | ncat -w2 localhost 4371
```

## Regenerating the patch

If our oracle changes need to be updated, edit files in `duckstation/` directly, then regenerate the patch against the pinned upstream base:

```bash
cd duckstation
git diff ffb33c281d196eb8ee0f559085ca285de7cdd51b > ../tools/duckstation/psxrecomp_oracle.patch
```

The pinned base SHA lives in one place only: `UPSTREAM_BASE` at the top of `tools/duckstation/setup.sh`. Update it there if we ever rebase onto a newer upstream commit.

## Why this layout (vs a hosted fork)

- Keeps the upstream source untracked in v4's git history — no 2.1GB of upstream code in our blame.
- Keeps *our* 1200-line patch reviewable as a single text diff in `tools/duckstation/` — in-tree, versioned, diffable across sessions.
- Matches the nestopia setup in sibling project `<nesrecomp>/runner\nestopia_cmake.cmake` + `runner/nestopia_oracle.patch`. Same mental model: submodule upstream, patch on top, auto-apply at setup time.
- No private/public GitHub fork to maintain or keep in sync.

## Protocol parity with native runtime

Both servers implement the same JSON-over-newline command set where possible so that `tools/debug_client.py compare <cmd>` diffs state between them. See `TCP_COMMANDS.md` at the v4 root for the full command table with "native-only / duckstation-only / both" annotations.

## First-time setup: the `duckstation-qt.rcc` resource file

The MSBuild `duckstation-qt` target does NOT include the Qt resource compile
step. You must also build the `duckstation-qt-rcc` target, which produces
`build/bin/resources/duckstation-qt.rcc` (~740 KB). Without it the binary
opens a dialog `"duckstation-qt.rcc could not be loaded. Your installation is
not complete."` and exits. `tools/duckstation/build.sh` builds both targets;
do not short-cut to `-t:duckstation-qt` alone.

After the rcc is in place, a **first-time GUI launch** is also required so
DuckStation's setup wizard can write out a working `settings.ini`. Launch
without any command-line flags:

```bash
python3 tools/launch_ds_detached.py   # spawns GUI, no flags
```

Click through the wizard (BIOS directory → pick `duckstation/build/bin/bios/`,
then continue, then quit). After that, headless launches work forever:

```bash
python3 tools/launch_ds_detached.py -bios -nogui -fastboot
```

Verify with a quick ping:

```bash
NC='/c/Program Files (x86)/Nmap/ncat'
(printf '{"cmd":"ping"}\n'; sleep 1) | "$NC" localhost 4371
# expect: {"id":0,"ok":true,"frame":N}  with N > 0
```

The settings.ini that the wizard writes is needed but not tracked (it's under
the gitignored `duckstation/build/`). If you rebuild from scratch (clobbering
`build/`), repeat the GUI-first-launch once.

## Troubleshooting

- **"oracle patch does not apply cleanly"** — either upstream commit has been moved past the pinned base (check `git -C duckstation log --oneline -1`), or the patch file was regenerated against a different base. Fix by either resetting the submodule to `$UPSTREAM_BASE` or updating the pin.
- **"sha256 mismatch" on deps archive** — the prebuilt deps version in `duckstation/dep/PREBUILT-VERSION` has been bumped. Either update `PREBUILT_SHA256` in `setup.sh` to the new hash from `duckstation/dep/PREBUILT-SHA256SUMS`, or pin the submodule to an older commit that expects the cached archive's version.
- **"UNSUPPORTED CONFIGURATION" warning from DuckStation CMake** — cosmetic. The upstream build system prefers `msbuild` driven via a VS solution (which is what `build.sh` does). The warning fires any time CMake runs on Windows; safe to ignore.
