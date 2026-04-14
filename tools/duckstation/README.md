# DuckStation oracle (dynamic comparison harness)

PSXRecomp v4 uses a patched build of [stenzek/duckstation](https://github.com/stenzek/duckstation) as a live oracle for the recompiled BIOS. Our runtime speaks JSON-over-TCP on port **4370**; the patched DuckStation speaks the same protocol on **4371**, so tools like `debug_client.py compare` can diff live state between the two at any moment.

## Layout

- **`duckstation/`** (git submodule) — pristine upstream DuckStation source tree, pinned to commit `ffb33c281d196eb8ee0f559085ca285de7cdd51b` (release-20260328 era). Never edited directly.
- **`tools/duckstation/psxrecomp_oracle.patch`** — our changes as a unified diff against the pinned upstream base. Touches 7 files (~1200 lines). Adds `src/core/psxrecomp_debug_server.{cpp,h}`, wires `PSXRecompDebug::Initialize(4371)` into `System::Initialize`, exposes three GPU debug accessors, registers a log channel.
- **`tools/duckstation/setup.sh`** — idempotent. Initializes submodule, fetches + verifies + extracts prebuilt Windows deps (SDL3, Qt6, ffmpeg, …), normalizes the absolute paths baked into the prebuilt CMake metadata, applies the oracle patch.
- **`tools/duckstation/build.sh`** — runs CMake (Visual Studio 17 2022, x64, Release) and MSBuild on `duckstation-qt`. Requires the Visual Studio 2022 "Desktop development with C++" workload (CMake + MSBuild come with it).
- **`tools/fix_duckstation_deps_paths.py`** — helper used by `setup.sh` to rewrite stale `_IMPORT_PREFIX` values in the extracted prebuilt deps.

## First-time setup

```bash
cd /f/Projects/psxrecomp-v4
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
- Matches the nestopia setup in sibling project `F:\Projects\nesrecomp\runner\nestopia_cmake.cmake` + `runner/nestopia_oracle.patch`. Same mental model: submodule upstream, patch on top, auto-apply at setup time.
- No private/public GitHub fork to maintain or keep in sync.

## Protocol parity with native runtime

Both servers implement the same JSON-over-newline command set where possible so that `tools/debug_client.py compare <cmd>` diffs state between them. See `TCP_COMMANDS.md` at the v4 root for the full command table with "native-only / duckstation-only / both" annotations.

## Known issue: rebuilt binary doesn't start emulation (2026-04-14)

The binary from this session's rebuild does not start BIOS emulation regardless
of launch flags (`-bios -nogui -fastboot`, `-bios -nogui -slowboot`, `-bios`
alone, `-bios -batch`, etc.). The process runs (stable ~34 MB, no growth), but
`PSXRecompDebug::Initialize(4371)` is never reached because it's called *after*
`System::Initialize()` completes, and `System::Initialize()` never completes —
likely `BIOS::GetBIOSImage` or a downstream step fails silently (the error path
in `QtCore::CoreThread::bootSystem` emits a signal that's lost in `-nogui`).

What's been verified working:
- Binary has all patch strings (`PSXRecompDebug`, `pc_break`, `read_ram`,
  `psxrecomp_debug_server`) — confirmed via `tools/check_ds_patch.py`.
- BIOS file present + valid: `duckstation/build/bin/bios/SCPH1001.BIN`
  has MD5 `924e392ed05558ffdb115408c263dccf` which matches DuckStation's known
  SCPH-1001 hash in `src/core/bios.cpp:36`.
- Prebuilt deps extracted + path-normalised (`tools/fix_duckstation_deps_paths.py`
  handles the 27-file `_IMPORT_PREFIX` rewrite).
- `settings.ini` has `[BIOS]PathNTSCU = SCPH1001.BIN`, `SearchDirectory = bios`,
  `[Main]SetupWizardIncomplete = false`, `SettingsVersion = 3`,
  `[AutoUpdater]CheckAtStartup = false`, `LogToFile = true`.
- `portable.txt` placed, `resources/` + `translations/` directories next to exe.
- Pinned at upstream commit `ffb33c281` (release-20260328 era).

What hasn't been ruled out:
- An environmental factor (GPU/Vulkan/D3D init failing silently for the user's
  particular GPU + Qt 6.11 combination on this specific machine).
- A subtle difference in the prebuilt deps between the archive's extraction time
  and now — the archive was extracted originally into `psxrecomp-projects/` and
  has absolute paths baked into CMake metadata; our path-rewriter fixes the
  CMake paths but there may be other baked paths (e.g., pdb references) we miss.
- Silent `InitializeAudio` / `GPU` / `Achievements` error paths in
  `System::Initialize` that would be visible only in a DuckStation log file —
  which itself is never written because log init happens after the failing step.

The pre-rebuild binary (the one I destroyed with `rm -rf`) worked flawlessly
for the same invocation. The same source, with only my uncommitted `pc_break`
additions on top, doesn't. Something in the environment changed between the
original build and the rebuild; without the original binary to diff against we
can't narrow it further from inside a session.

**Workaround / recovery path for a next session:**

1. Reproduce the failure: `bash tools/duckstation/build.sh` then launch.
2. Enable DuckStation's console logging via the Qt debugger or by editing
   `src/core/system.cpp` to add a `DEV_LOG(...)` around each gate in
   `BootSystem`, rebuild, identify which gate fails, push the log output into a
   TCP-visible place (per `CLAUDE.md` §3: no `fprintf` to stderr).
3. Or: run the prebuilt-released DuckStation binary from GitHub against the
   same BIOS to confirm the environment setup is fine, then narrow to the
   patch-vs-upstream delta.

The restructure itself is complete and correct; only the "post-rebuild binary
works end-to-end" box is un-ticked. The oracle can still be driven interactively
through the GUI debugger window — that's how DuckStation is designed to be used.

## Troubleshooting

- **"oracle patch does not apply cleanly"** — either upstream commit has been moved past the pinned base (check `git -C duckstation log --oneline -1`), or the patch file was regenerated against a different base. Fix by either resetting the submodule to `$UPSTREAM_BASE` or updating the pin.
- **"sha256 mismatch" on deps archive** — the prebuilt deps version in `duckstation/dep/PREBUILT-VERSION` has been bumped. Either update `PREBUILT_SHA256` in `setup.sh` to the new hash from `duckstation/dep/PREBUILT-SHA256SUMS`, or pin the submodule to an older commit that expects the cached archive's version.
- **"UNSUPPORTED CONFIGURATION" warning from DuckStation CMake** — cosmetic. The upstream build system prefers `msbuild` driven via a VS solution (which is what `build.sh` does). The warning fires any time CMake runs on Windows; safe to ignore.
