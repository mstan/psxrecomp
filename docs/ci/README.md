# CI helpers for setup-host releases

Shared scripts and GitHub Actions so every PSXRecomp title uses one
standardized release flow.

**Start here:** [`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md)

## Workflow template

**New Project Layout** (`tools/new_project_layout/setup_project.{sh,ps1}`) copies
this template into the title repo and replaces `YOUR_*` / `yourgame-release`
using `--zip-prefix` (or a derived acronym) plus the window title. It also
writes `scripts/package_setup_release.sh`, `README.md`, and an optional
`framework_pins.txt` snapshot, and can opt in wizard/netplay via
`--enable-wizard` / `--enable-netplay`. Release CI pins via submodule gitlinks
and only logs SHAs with `record_pins.sh` (no `verify_pins` gate).

**Migrating an older title** onto this CI shape: Project Studio
(`tools/new_project_layout/migrate_project.py apply` / `gui`) emits the same
packager + filled `release.yml`. Setup-host only — no prebuilt game-C zips.

Manual:

```bash
mkdir -p .github/workflows
cp psxrecomp/docs/ci/templates/setup-release.yml .github/workflows/release.yml
# edit YOUR_* placeholders — or use fill_tokens.py --ci-placeholders
# Release name is YAML-quoted so titles with ':' (e.g. Marvel vs. Capcom: …) parse.
```

Template: [`templates/setup-release.yml`](templates/setup-release.yml)

**Versioning:** `workflow_dispatch` with an empty `version` auto-bumps the next
`X.Y.Z` from the latest `v*.*.*` git tag (`bump`: patch / minor / major; default
patch). Pass an explicit version to override. Shared helper:
`tools/ci/normalize_version.sh --next [patch|minor|major]`.


**Host-only model / player updates:** [`HOST_ONLY_RELEASES.md`](HOST_ONLY_RELEASES.md)
(CI never ships game C; RetComM Update reuses codegen-cache for host/UI bumps;
`reuse_cached_emitters` + ccache speed up release jobs when the `psxrecomp` pin
is unchanged).

Matrix: `ubuntu-24.04` (linux-x64), `windows-2022` (windows-x64),
`macos-15` (macos-arm64), `macos-15-intel` (macos-x64).

Release hosts always configure with Vulkan headers + `glslc` (MSYS2 /
apt / Homebrew packages), verify `vulkan.h` + `glslc` before configure, and
fail the job if CMake does not select `PSX_HAVE_VULKAN`. Runtime still loads
the ICD dynamically via SDL; CI only needs headers and the shader compiler.

## Tools under `psxrecomp/tools/`

| Script | Role |
|--------|------|
| `ci/normalize_version.sh` | Normalize / write `VERSION` + `TAG`; `--next` auto-bumps from latest tag |
| `ci/clear_generated.sh` | Clear `generated/` for setup-host CI |
| `ci/record_pins.sh` | Log `psxrecomp` / `recomp-ui` / `recomp-net` SHAs (CI + scaffold) |
| `ci/verify_pins.sh` | Optional local check vs `framework_pins.txt` (not used by release CI) |
| `ci/build_emitters.sh` | Build `psxrecomp-game` + `psxrecomp-bios` |
| `fetch_toolchain.sh` | Download/unpack cmake-clang-v1 (Windows emitter builds; optional embed) |
| `stage_setup_sdk.sh` | Emitters, OpenBIOS, optional `toolchain/`, MinGW DLLs |
| `bundle_mingw_dlls.sh` | Copy imported non-system DLLs next to Windows PEs |
| `package_setup_host.sh` | Lean setup-host zip (optional `--embed-toolchain`) |
| `../cmake/toolchain-mingw-w64.cmake` | Linux→Windows MinGW cross toolchain |
| `../host/psxrecomp_codegen_host.*` | Portable Generate & rebuild host (via CMake helper) |
| `templates/game.gitignore` | Suggested gitignore for title repos |

Also use `psxrecomp_add_game_runtime(...)` in `runtime/runtime.cmake` for
setup-host / full-game wiring. The CLI (`psxrecomp_cli.py`) lives in this
submodule — there is no separate `psxrecomp-sdk/` overlay.

## Composite actions

```yaml
- uses: ./psxrecomp/.github/actions/build-emitters

# Prefer package_setup_host.sh (allow-no-toolchain by default).
# fetch-toolchain + --embed-toolchain only for offline-first packs.
- uses: ./psxrecomp/.github/actions/stage-setup-sdk
  with:
    stage: dist/stage-setup-${{ matrix.artifact }}
    recompiler-build: build-recompiler
    allow-no-toolchain: 'true'
    runtime-bin: /mingw64/bin   # Windows / MSYS2
```

## Title responsibilities

Keep only this in the game repo:

- Setup-host CMake flags and exe basename
- Thin `codegen_setup.c` / `.h` with `psx_game_codegen_forward_if_built`
  (scaffold: `tools/new_project_layout/templates/codegen_setup.c.in`)
- For netplay titles: `PSX_NETPLAY ON` **before** `include(runtime.cmake)`
  (`ENABLE_NETPLAY_IF_PRESENT` alone is too late), plus `-DPSX_NETPLAY=ON`
  in the release workflow configure step
- Thin `scripts/package_setup_release.sh` wrapping `package_setup_host.sh`
- Zip prefix / display name / disc hint in that wrapper
- Release notes / GitHub Release job naming

Release CI configures with `-DPSXRECOMP_FORCE_SETUP_HOST=ON`,
`-DPSXRECOMP_ALLOW_NO_BIOS=ON`, and `-DPSX_SETUP_WIZARD=ON` after
`clear_generated.sh`. Titles must also set `PSX_SETUP_WIZARD` /
`ENABLE_SETUP_WIZARD` in `CMakeLists.txt` — without the wizard flag the
setup-host zip never opens first-run / Generate & rebuild. The zip ships
emitters + OpenBIOS.toml; end users Generate (OpenBIOS always; SCPH1001 if
they have a dump) via the wizard, then rebuild.

## Windows code signing

Windows 11 **Smart App Control** (on by default on new installs) allows an
executable only if it carries a valid Authenticode signature or its exact
hash already has reputation. Every CI build is a new hash, so an unsigned
release is blocked outright, with no "run anyway".

`package_setup_host.sh` therefore runs `tools/ci/sign_windows.sh` over the
staged tree before zipping. It signs the host exe, every DLL, and the
emitters (an embedded `toolchain/` is left alone) with SHA-256 and an RFC 3161
timestamp, then verifies each file.

| Secret / variable | Meaning |
|---|---|
| `WINDOWS_SIGN_PFX_BASE64` | the PKCS#12 certificate, base64 (`base64 -w0 cert.pfx`) |
| `WINDOWS_SIGN_PFX_PASSWORD` | its password (omit for a passwordless .pfx) |
| `WINDOWS_SIGN_TIMESTAMP_URL` | optional; default `http://timestamp.digicert.com` |
| `WINDOWS_SIGN_DESCRIPTION` | optional; text shown in the file's properties / UAC |

**No certificate = no signing, and the build still succeeds** (a notice is
printed). A certificate that is present but fails to sign is fatal. The
workflow template passes the two secrets to the *Package setup zip* step;
forks without them package unsigned exactly as before.

Certificate choice: an OV certificate satisfies Smart App Control at once,
while SmartScreen reputation still builds over downloads. An EV certificate
or Azure Trusted Signing is trusted by both immediately. Trusted Signing
does not hand out a .pfx; wiring it means a different signing step, not
this script.

What signing cannot cover: a setup-host title is **rebuilt on the player's
machine** after Generate. That locally built game exe is a new, unsigned
binary and Smart App Control judges it on its own. Signing covers the
shipped host/wizard, emitters and DLLs; a machine with Smart App Control
*On* will still refuse the locally built game until the policy is turned
off (Windows Security → App & browser control), which is permanent.

## Release checklist

See [`../GAME_PROJECT_SETUP.md`](../GAME_PROJECT_SETUP.md#bundled-release-checklist).
