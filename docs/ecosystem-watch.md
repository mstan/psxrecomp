# PSXRecomp ecosystem watch

This is a recurring upstream/fork audit, not a merge queue. It records the
exact external state that was inspected, separates reusable framework work from
title-specific work, and prevents an old fork from silently replacing newer
framework fixes.

> Note: entries evaluated on or before 2026-07-13 predate the removal of the
> sljit Tier-2 backend (`5b7e69b4`, 2026-07-15). Where an entry describes an
> external branch touching "SLJIT", that surface no longer exists here; the
> equivalent path today is the bundled tcc tier plus the dirty interpreter.

## Audit baseline and rules

- Evaluated through `2026-07-13T21:25:28-07:00` (America/Los_Angeles), against `mstan/psxrecomp`
  `master` at `dde268dc0fb9daf8fe6529f4aebfe80995350334` (committed
  `2026-07-13T12:54:28-07:00`).
- Investigation worktree: `audit/ecosystem-2026-07-13`.
- The initial audit inspected diffs and history without importing external
  code or reproducing the contributors' game builds. Focused follow-up ports
  are tracked below; the audit branch itself remains documentation-only.
- A candidate must be game-agnostic or explicitly configured by the game repo,
  hardware-backed where it claims correctness, independently testable, and one
  focused PR. Hard-coded title behavior, generated output, local paths, and
  `printf`/log diagnostics do not belong in the framework.
- Cherry-picking preserves the original author. If a change must be
  reconstructed, use the contributor's exact `Co-authored-by` trailer and cite
  the source PR and commit in the PR description.

## Evaluation ledger

Update a row whenever its source head changes. `Metadata only` means the source
was discovered but its diff was not evaluated.

| Source | Owner | Evaluated ref and commit | Commit time | Last evaluated | State |
|---|---|---|---|---|---|
| [PR #13: stb launcher and accumulated runtime work](https://github.com/mstan/psxrecomp/pull/13) | NyperYuhgard | `feat/launcher-stb-revamp` `812ce6f50c52c5c0fe86adf74ad302f3063b4f7f` | `2026-07-13T16:47:04-05:00` | `2026-07-13` | Fully inspected; split required |
| [PR #14: WebAssembly and compatibility](https://github.com/mstan/psxrecomp/pull/14) | Kareem Olim (`kem0x`) | `agent/add-web-runtime-and-compatibility-fixes` `a99a8fc14e26be3c48806ca85f15068a2c6801ae` | `2026-07-14T04:30:04+03:00` | `2026-07-13` | Fully inspected; split required |
| [PR #15: config-driven game tweaks](https://github.com/mstan/psxrecomp/pull/15) | `douglasjv` | `agent/game-tweak-framework` `31cdfb4f4dac305e98661dfdbf8e685533fd7ca4` | `2026-07-11T15:56:20Z` | `2026-07-13` | Fully inspected; see Douglas section |
| [PR #16: SmackDown 2 framework changes](https://github.com/mstan/psxrecomp/pull/16) | Martin Penkava (`shaneomac1337`) | `smackdown2-fixes` `d05d14bb11caafad2963abe824f249249a80cb11` | `2026-07-12T19:16:19+02:00` | `2026-07-13` | Fully inspected; split required |
| [douglasjv/mm8](https://github.com/douglasjv/mm8) | `douglasjv` | `main` `4eae0f952df9a527d18ff1916d373619fbb1a391` | `2026-07-13T21:28:50Z` | `2026-07-13` | Fully inspected; see Douglas section |
| [douglasjv/r4](https://github.com/douglasjv/r4) | `douglasjv` | `main` `e4262718269879ef2e9b11e2fa428301082117b8` | `2026-07-13T15:22:57Z` | `2026-07-13` | Fully inspected; see Douglas section |
| [douglasjv/psxrecomp-tweaks](https://github.com/douglasjv/psxrecomp-tweaks) | `douglasjv` | PR branch above; fork default `master` is `0cb2a77bff02e90f25d2791072a94b6ce73039f4` | `2026-07-09T20:50:09Z` | `2026-07-13` | Fully inspected; default branch is 59 commits behind baseline |
| [wowjinxy/psxrecomp](https://github.com/wowjinxy/psxrecomp) | `wowjinxy` | `master` `46c664713c12cf719b3b5e44aa18eea63dca13af` | `2026-06-17T23:32:22Z` | `2026-07-13` | Metadata only; 8 ahead / 402 behind |
| [seedlord/psxrecomp](https://github.com/seedlord/psxrecomp) | `seedlord` | `master` `4ea9ecd2a2d9db60d23e39091a53f12a2b2e751b` | `2026-06-12T22:29:02Z` | `2026-07-13` | Metadata only; 467 behind, no unique commits |
| [chrisking1981/psxrecomp_kula_world](https://github.com/chrisking1981/psxrecomp_kula_world) | `chrisking1981` | `master` `80c33a8903c36df44d331b8a38ad64e591fd7b0a` | `2026-06-26T21:03:42Z` | `2026-07-13` | Metadata only; merged contribution branches already tracked in upstream PRs |
| [Creesic/psxrecomp](https://github.com/Creesic/psxrecomp) | `Creesic` | `master` `3c2b87cb883d67db984fcabed30c5195f39ee3cd` | `2026-03-28T02:33:18Z` | `2026-07-13` | Metadata only; legacy history has no common ancestor with v4 |

The fork comparison counts above are snapshots, not quality judgments. Old game
repos are expected to pin old framework commits; the point is to avoid copying
their whole framework back over current `master`.

## Consolidated ecosystem gaps

The strongest missing shared capabilities found in this pass are:

1. Faithful CD-DA playback and CUE pregap/index behavior. Current master already
   has multi-file CUE support, but not the complete Red Book playback path in
   PR #14.
2. A supported WebAssembly target: reproducible Emscripten toolchain, browser
   audio, persistence/input shell, hosting requirements, and a diagnostic story.
3. Declarative, opcode-verified game enhancement patches and typed widescreen
   extension points, so title repos stop editing framework source.
4. Cross-platform, source-only game-repo scaffolding with an exact framework pin
   and repeatable extraction/regeneration steps.
5. Renderer and SPU accuracy work that is presently being discovered during
   individual game bring-up: exclusive triangle edges, complete primitive-size
   rejection, SPU end/mute behavior, Gaussian interpolation, and reverb.
6. A first-class downstream compatibility signal. A game repo currently pins a
   commit, but there is no automated report saying how far behind it is, which
   framework capabilities it carries locally, or whether its pin has a known
   security/correctness supersession.

## NyperYuhgard branch / PR #13

Snapshot: 13 branch commits, 35 changed files, `+8,631/-1,914`; six current
master commits are absent, the PR conflicts, and it has no checks. The PR body
describes a three-commit launcher replacement, but the branch has since
accumulated CD, SPU, overlay, recompiler, tooling, documentation, FPS, traps,
and Crash Bash diagnostics.

### Keep under review

- The older in-tree launcher replacement remains blocked by missing GL loader
  and visual/layout review prerequisites. Do not use its runtime changes to
  justify merging launcher code.
- `a39ab37` contains Linux shared-library overlay discovery, ABI preflight, and
  cache loading work. Re-derive the smallest useful parts on top of master's
  newer exact overlay dispatch (`b39387e`) and test save/load plus stale-cache
  behavior.
- `b49548e` adds an FPS counter and overlay status. Keep only product-facing
  telemetry that uses the normal UI/debug surfaces.
- `b24d685` adds runtime flag and debug-command indexes. Regenerate or verify
  them against current master before a docs-only PR.
- The dirty-text write path in `b07cb79` also marks the page dirty, closing a
  plausible gap where native dispatch is blocked but interpreter dispatch never
  activates. Isolate and oracle-test that few-line behavior.

### Do not upstream as written

- The branch's CD/multi-file reader is older than and superseded by master's
  `5a2c6fd` implementation.
- The simplified floating-point reverb is not a faithful PS1 SPU model.
- Hard-coded Crash Bash write addresses, large `stderr` crash dumps, and local
  log/watchpoint behavior conflict with the framework's game-agnostic and TCP
  observability rules.
- The recompiler additions accept branch-likely and `MOVN`/`MOVZ` instructions
  that are not part of the PS1's MIPS I CPU, risking data being classified as
  code. Its BC2T decoding also treats `rs=9` as BC2T; COP2 branches use the BC
  format under `rs=8` with the condition in `rt`. This code needs a new ISA-led
  design, not a transplant.
- The shell build helpers are thin local conveniences rather than the
  cross-platform documented build workflow.

## Kem0x branch / PR #14

Snapshot: 12 branch commits (10 feature commits and two upstream merges), 34
files, `+1,991/-395`, clean against the audit baseline, but no reviews, CI
checks, or added regression tests.

### High-value isolated fixes

- `dd9e5c6`: CD interrupt matching should test the encoded low interrupt bits
  with `irq_enable & irq_flag`. This is a small hardware-backed correctness PR.
- Generic subset of `b2597df`: an ADPCM END without REPEAT jumps to the repeat
  address, sets ENDX/Release, and immediately mutes the envelope. Exclude the
  Pepsiman duplicate-effect timing guard.
- `91a654f`: exclusive bottom/right software-rasterizer edge coverage. Add
  renderer regressions for winding, clipping, adjacent triangles,
  semitransparency, and flat top/bottom cases.
- CD-DA/pregap subset of `0e9df70`: layer 2,352-byte audio sector streaming,
  track transitions, Play/GetlocP/autopause/DataEnd, and save-state coverage on
  top of master's reader. The PR currently omits `cdda_data_end_pending` from
  the serialized CD state.

### Larger capabilities

- `78128bf`, `9d8550c`, and `67207b4` establish an Emscripten pthread runtime,
  WebAudio bridge, stable key mapping, and vblank-safe pause. Before calling it
  supported, pin the toolchain, document a reproducible build and
  cross-origin-isolated hosting, supply the browser persistence shell, revisit
  the fixed 384 MiB/1 GiB memory policy, and retain useful diagnostics.
- Generic portions of `ec889d6` explore subpixel projection provenance,
  perspective-correct software texturing, and duplicate-frame presentation.
  They instrument generated code and hot stores and must be separated into
  experiments with performance and image-quality evidence.

### Already superseded or title-specific

- `01155a6` exact compiled ranges are absorbed and broadened by master's
  `b39387e`; the head also drops a newer Crash Bash clean-text fallback. Never
  merge the branch wholesale.
- Hard-coded Pepsiman/SLPS-01762 RAM addresses, menu chord injection, lives,
  level restart, and track-based controls belong in its game repo. The
  `PEPSIMAN_SFX_RETRIGGER_GUARD` masks a timing issue and does not belong in the
  shared SPU.

## Martin Penkava branch / PR #16

Snapshot: seven commits, 18 files, `+1,129/-96`, and six master commits behind.
The game build was reported successful, but there are no checks and the generic
BIOS-only framework link is reported failing.

### Candidate work

- `df4cc5c`: MSVC portability for bit scans, C linkage, and generated static
  initializers. Validate generated BIOS and game units with MSVC plus the
  existing GCC/Clang matrix.
- The `s_active` guards from `e6809cc` are a small overlay-loader lifecycle fix.
  The same commit's "decodable word above overlay floor" discovery fallback is
  much riskier and needs independent proof; it can classify data as executable.
- `cfc5d0e`: extend the PS1 1023x511 primitive-size rejection to every polygon,
  quad half, line, and polyline path. Master has a partial triangle reject, so
  port and test the missing coverage rather than replacing `gpu.c` wholesale.
- Split Gaussian interpolation from reverb in `e069c8a`. Verify the 512-entry
  table's provenance/licensing and compare both output and state transitions
  with the in-tree Beetle oracle before import.
- The Vulkan synchronization, staging-buffer reuse, coalesced submission, and
  presentation work may be useful after the known AMD corruption is resolved.
  Keep performance/synchronization fixes separate from CRT shaders and debug
  environment flags.

### Follow-up credit

PR #61 reconstructed the reusable Vulkan synchronization, staging-buffer,
command batching, present-mode, and timing work from Martin Penkava's PR #16
branch onto current master while leaving Vulkan experimental and hidden by
default. Martin Penkava (`shaneomac1337`) is the source developer for that
Vulkan work.

### Reject or relocate

- `4517bd5` labels the AMD corruption unresolved and carries temporary Vulkan
  diagnostics; it is not merge-ready.
- The three batch scripts hard-code one contributor's Visual Studio and
  `D:\tools` layout, and two directly regenerate SmackDown 2. They belong in a
  game repo or should be replaced by parameterized build documentation.
- The new shader describes itself as "CRT-Royale-inspired" while master already
  has attributed CRT/composite/Trinitron color models. Establish exact
  provenance and license, then evaluate a shader as a separate presentation
  feature.

## Douglas game repos and tweak framework

### Revision shape

Both game repos pin Douglas's framework fork as a submodule, and both pins are
based on upstream `c94fcd5` rather than current master:

| Game | Game head | Framework pin | Compared with audit baseline |
|---|---|---|---|
| Mega Man 8 | `4eae0f952df9a527d18ff1916d373619fbb1a391` | `3c316fc781ba645cca60125211299de0e879d770` | Pin has 16 unique commits; master has 6 absent commits |
| R4: Ridge Racer Type 4 | `e4262718269879ef2e9b11e2fa428301082117b8` | `19233f496fe810fb57afc226b4ff4509c812bc8f` | Pin has 10 unique commits; master has 6 absent commits |

Those six master commits include multi-file CUE support, exact native/static
overlay dispatch, audio reserve/catch-up hardening, release hygiene, and analog
stick/D-pad decoupling. Every port must start from `dde268dc`; neither game pin
is a safe integration base.

### Reusable game-project pattern

The repos are good source-only title skeletons: top-level CMake using
`psxrecomp_add_runtime_target`, a minimal seed, typed `game.toml`, exact
framework pin, CHD/PS-X EXE extraction, macOS prerequisites/build helpers,
research notes, and aggressive ignores for discs, BIOS, generated output,
caches, and runtime state. Neither has CI.

That repeated structure is evidence for an official game-project template or
scaffolder with standardized extract, configure, build, pin-status, and smoke
test jobs. Reimplement the workflow in a licensed upstream template; do not
copy the standalone repos' unlicensed scripts.

### Mega Man 8 findings

- The source-only project targets SLUS-00453. Its title-owned config uses
  reachable discovery, a verified smaller analysis bound, full-2D native wide,
  a scene-word gate, packet-proven HUD range, three background-2D sites, and a
  ring-refill option. Addresses, hashes, state gates, and policy stay in `mm8`.
- Its baseline notes report that whole-image discovery grew from 1,975
  candidates to 6,815 entries, 5.4 million C lines, and 176 MiB of generated C,
  while reachable discovery produced 132 entries and roughly 87,000 lines.
  That is strong motivation for a bounded/reachable discovery API.
- The documented real-disc/headless Debug run reached controllable opening-stage
  play for 17,425 consecutive frames with no unknown dispatch; Release linked.
  The repo correctly rejected a guessed 60 FPS patch after motion/timing
  evidence contradicted it. This is useful integration evidence, but no CI
  reproduces it.
- Framework pin `3c316fc` extends existing background-2D support with a gameplay
  RAM gate, ring-refill/signed-column behavior, interpreter parity, and SLJIT
  decline for affected fragments. Upstream only generic typed pieces and test
  4:3 identity plus mutated opcodes; do not import the MM8 addresses.

### R4 findings

- The source-only project targets SLUS-00797. Its title config owns race callback
  split addresses, projection squash, cull widening, hybrid analog, and exact
  polygon/HUD packet sources. `game-android.toml` selects a reduced software
  rendering profile.
- The repo records 2,982 strict functions, Apple Silicon Release linking, real
  LLE boot/attract, GL 4.1, 60 simulation steps and 60 unique frames in two
  seconds, correct wall-clock timers, and live analog bytes. Fourteen HUD
  evidence PNGs are tracked. Android build/install material exists, but there
  is no documented device result.
- Commit `0f10098` adds configurable polygon/line HUD regions guarded by packet
  source, bounds, and pivot. It is inert unless configured and has unusually
  concrete visual evidence; it is a good standalone typed extension after
  parser, bounds, and default-identity tests.
- Commit `7abbfdf` mixes a generic frame-split callback mechanism with POSIX
  overlay loading, interpreter-page controls, and SPU CD-capture/IRQ changes.
  The frame split is promising but high-risk: review save/reset state, nested
  callbacks, interrupts, cycles, audio, and replay before a design branch.
- The same ancestry supplies the missing POSIX cache scan and manifest-driven
  `dlsym(func_%08X)` registration. Current master still logs that POSIX export
  scanning is TODO. Rebase this narrowly onto the current loader, validate both
  address and CRC, ABI cleanup, rescan, and handle lifetime on Linux and macOS.
- Android support adds an arm64 SDLActivity target, app-private paths, software
  renderer stubs, AArch64 fiber context switching, and raw SDK/NDK packaging.
  It is API-26/arm64/debug-only, clones SDL during build, exports the activity,
  and lacks controller/audio/suspend/device evidence. Treat it as an experiment,
  splitting runtime portability from APK tooling.

### PR #15 findings

PR #15 is six commits, 23 files, `+644/-25`; current master has nine unique
commits not in it. A synthetic merge was clean, but the PR has no checks or
automated tests. It introduces three useful inert-by-default configuration
families:

1. `[[recompiler.patch]]`: named address/expected/replacement patches with
   duplicate checks, fatal main-EXE mismatch, and guarded overlay variants.
2. `[[widescreen.signed_x_bound]]`: signed Q16 gameplay bounds wired through
   static generation, captured overlays, SLJIT, and the interpreter.
3. Typed textured-edge expansion/full mirror options for finite arena and
   background geometry.

It also bundles macOS GL 3.3 core context setup, CPU-copied overlay discovery,
analog debug injection/observability, client commands, and a thorough
`GAME_TWEAKS.md`. The overall architectural direction is strong: exact guest
instruction changes remain game data, typed framework behavior is off by
default, and the shared code contains no title addresses.

Fix these before proposing merge:

- Software rendering treats the textured-edge return code as a boolean and
  stretches the whole primitive, unlike the GL-specific outside-edge meaning.
- Runtime signed-bound registration silently truncates after 64 sites while
  codegen accepts more, allowing backend divergence.
- Signed-bound scaling includes cull-guard pixels through
  `psx_ws_x_margin()`, coupling a gameplay bound to renderer safety margin.
- Treating every dispatch above `g_overlay_region_floor` as dirty is broad and
  can turn data targets into interpreter candidates.
- Loading the same config through `--config` and `--ws-config` can append
  duplicates without cross-source validation; address alias semantics and the
  linear per-instruction patch lookup also need definition.
- Split patches, signed bounds, textured edges, platform GL, overlay discovery,
  debug input, and final docs into focused reviews.

PR #15's public body reports AppleClang/Ninja, Tobal generation/runtime link,
Einhander regeneration/frame tests, and Mega Man Legends generation/link/boot.
No attached checks or artifacts independently reproduce those claims. MM8 and
R4 descend from the PR but do not currently consume its guarded-patch,
signed-bound, or textured-edge entries; they instead demonstrate that the typed
profile pattern is continuing to grow.

## First-wave implementation branches

The first wave was rebuilt in isolated worktrees from current `master`, reviewed
through `2026-07-13T22:00:00-07:00`, and initially published as draft PRs. Each branch
has an in-tree `docs/internal/upstream/` record linking its exact source,
preserving human credit, and listing related material deliberately excluded.

| PR / branch | Implementation commit | Evaluated source | Last evaluated | Validation / state |
|---|---|---|---|---|
| [#20 `fix/cdrom-encoded-interrupts-kem0x`](https://github.com/mstan/psxrecomp/pull/20) | [`52eb8e8a`](https://github.com/mstan/psxrecomp/commit/52eb8e8a24ca3715afc835b1994f69e269bf4796) | [PR #14 `dd9e5c6`](https://github.com/mstan/psxrecomp/commit/dd9e5c65073dd41fbac2d965d2f945a866ec59e2) | `2026-07-13` | Focused mask test and standalone compile pass; Kareem co-author |
| [#17 `fix/spu-end-mute-kem0x`](https://github.com/mstan/psxrecomp/pull/17) | [`76ddcc00`](https://github.com/mstan/psxrecomp/commit/76ddcc008a72b1dbb8d9bee227e02ae6549caa99) | [PR #14 `b2597df`](https://github.com/mstan/psxrecomp/commit/b2597df6d5905a0e183b5309a9b84720cd417e19) | `2026-07-13` | Runtime behavior already on master; new END/REPEAT regression passes; Kareem co-author |
| [#23 `fix/overlay-init-guard-shaneomac`](https://github.com/mstan/psxrecomp/pull/23) | [`5b8098a9`](https://github.com/mstan/psxrecomp/commit/5b8098a9d997424cbd57829b8ae11369ab06935a) | [PR #16 `e6809cc`](https://github.com/mstan/psxrecomp/commit/e6809ccf7d778a4c2f32d9e27c0ec31a44cbd2ba) | `2026-07-13` | Pre-init regression and standalone compile pass; source co-authors preserved |
| [#21 `fix/msvc-generated-init-shaneomac`](https://github.com/mstan/psxrecomp/pull/21) | [`05d4f35b`](https://github.com/mstan/psxrecomp/commit/05d4f35bd6314d56ac95e6cec4b90655950dbd86) | [PR #16 `df4cc5c`](https://github.com/mstan/psxrecomp/commit/df4cc5c70ab1e5d0fd2a8f817dd346905d4d0eeb) | `2026-07-13` | MSVC game/BIOS/test builds, 44/44 L2, and 63 runtime objects; original author preserved |
| [#18 `fix/posix-overlay-export-scan-douglas`](https://github.com/mstan/psxrecomp/pull/18) | [`c0fa9b1e`](https://github.com/mstan/psxrecomp/commit/c0fa9b1e89fb6c210011dc25a92b124df43692d4) | [`df7d1fa`](https://github.com/douglasjv/psxrecomp-tweaks/commit/df7d1faa5f27be0ba357463a763c77efc43e1f91), [`7abbfdf`](https://github.com/douglasjv/psxrecomp-tweaks/commit/7abbfdf38b488d3764b96c155549bc930e0521b6) | `2026-07-13` | Real Linux shared-library fixture and Windows compile pass; merged with author approval |
| [#22 `feat/config-guarded-mips-patches-douglas`](https://github.com/mstan/psxrecomp/pull/22) | [`5f62deda`](https://github.com/mstan/psxrecomp/commit/5f62dedaa3bddfdae6903524449215caa0b612f5) | [PR #15 `19001c0`](https://github.com/douglasjv/psxrecomp-tweaks/commit/19001c0a383edde439988b93eb2385e2d789d350) | `2026-07-13` | Patch CTest 2/2 (including C++17 public-header coverage), L2 44/44, combined game builds; merged with author approval |
| [#19 `feat/reachable-main-discovery-douglas`](https://github.com/mstan/psxrecomp/pull/19) | [`bd705bfc`](https://github.com/mstan/psxrecomp/commit/bd705bfcecff91a3c0832ccd01b0504c54b100b1) | [MM8 `8415f3a`](https://github.com/douglasjv/mm8/commit/8415f3a95458c41c7f48fbe36caaf0ee82730720) | `2026-07-13` | Parser/codegen tests, L2 44/44, overlay/static guard pass; merged with author approval |

The Douglas-derived changes were merged with author approval.

## Disposable combined regression exercise

The PR branches were combined only in the unpushed local branch
`test/ecosystem-wave-integration` at `bed0c08048b954f7f76d6a44bd97ce1eb40688f8`.
This is a test fixture based on unchanged `master` `dde268dc`; it is not a
merge proposal. The exercise was evaluated through
`2026-07-13T23:53:52-07:00` (America/Los_Angeles).

| Game repository / test ref | Framework under test | Last evaluated | Regeneration/build | Runtime acceptance |
|---|---|---|---|---|
| Tomba 1 `05e058f` | local combined fixture `bed0c080` | `2026-07-13` | External local disc located, `SCUS_942.36` freshly extracted, 6,245-entry regeneration and launcher-free Debug runtime build passed | Visible manual pass reported: attract, gameplay, and save loading |
| Tomba 2 `3562540` | local combined fixture `bed0c080` | `2026-07-13` | Fresh regeneration and Debug runtime build passed | 328 s, 4,477 frames, 4,477 VBlanks raised, 22,898 IRQs; no fatal heartbeat, crash report, or error-pattern log hit; follow-up framebuffer showed active gameplay/tutorial content |
| Mega Man X6 `66d0f16` | local combined fixture `bed0c080` | `2026-07-13` | Fresh regeneration and launcher-free Debug runtime build passed | 328 s, 4,400 frames, 4,401 VBlanks raised, 59,918 IRQs; no fatal heartbeat, crash report, or error-pattern log hit; follow-up framebuffer showed the active intro sequence |
| Ape Escape `3367eb1` | local combined fixture `bed0c080` | `2026-07-13` | Fresh regeneration and launcher-free Debug runtime build passed | Initial parallel run: 328 s, 5,231 frames, 5,231 VBlanks raised, 25,405 IRQs, clean. Isolated follow-up: 4,418 frames, clean, framebuffer showed active intro/demo content |

The user additionally completed visible manual attract, gameplay, and save-load
smoke checks across all four games. The simultaneous three-game pass was
subjectively sluggish under host contention, so it is compatibility evidence,
not a performance comparison.

The soaks used `--headless --no-launcher --renderer software`, isolated working
directories, and distinct debug ports. They cover guest execution, disc/CD,
SPU timing, interrupts, overlay loading, and unattended title/attract
progression, but are not visual-renderer or audible-output acceptance tests.
The game worktrees contain only temporary path/junction wiring and regenerated
output for this exercise; no game-repository change is proposed.

A second three-process framebuffer-check run exposed one load-sensitive test
harness anomaly: Ape Escape's wall-clock starvation watchdog aborted at frame
268 after the process received no heartbeat opportunity for 6.59 seconds. It
had completed the earlier parallel soak, and it immediately passed the isolated
4,418-frame rerun above. Treat this as host-contention evidence when scheduling
parallel soaks, not as a reproducible guest or PR-branch failure; retain the
crash report and starvation dump in the temporary Ape test worktree.

## Remaining upstream branch queue

The completed first-wave branches are recorded above. The remaining queue is
kept narrow so each source and exclusion decision receives its own review.

| Priority | Proposed branch | Source credit | Scope / gate |
|---|---|---|---|
| 2 | `fix/sw-raster-exclusive-edges-kem0x` | Kareem Olim, `91a654f` | Edge rules; image/oracle suite |
| 2 | `fix/gpu-primitive-size-reject-shaneomac` | Martin Penkava, `cfc5d0e` | Complete existing partial hardware rule |
| 2 | `fix/dirty-text-page-mark-nyper` | NyperYuhgard, subset of `b07cb79` | Dirty/native dispatch invariant; no Crash Bash logging |
| 2 | `feat/cdda-playback-kem0x` | Kareem Olim, subset of `0e9df70` | Extend master's CUE reader; save-state and track tests |
| 2 | `feat/widescreen-hud-regions-douglas` | Douglas, `0f10098` | Typed polygon/line HUD regions; GL/SW identity tests |
| 2 | `feat/widescreen-signed-bounds-douglas` | Douglas, `f44f365` | Remove 64-site/cull-margin hazards; all backend tests |
| 2 | `fix/macos-core-gl-context-douglas` | Douglas, subset of `df7d1fa` | Context setup only; macOS launcher/game smoke tests |
| 3 | `feat/web-runtime-core-kem0x` | Kareem Olim, `78128bf` | Reproducible platform target, no title hooks |
| 3 | `feat/web-runtime-controls-kem0x` | Kareem Olim, generic web-control commits | Keybind/pause API and persistence contract |
| 3 | `feat/game-project-template` | Douglas-inspired workflow | New licensed template/scaffolder; pin-status + CI |
| Hold | Existing `feat/launcher-stb-revamp` | NyperYuhgard | First solve GL loading and visual acceptance |
| Hold | `exp/vulkan-frame-delivery-shaneomac` | Martin Penkava | Only after AMD corruption and provenance gates |
| Design | `design/runtime-frame-split-douglas` | Douglas, generic subset of `7abbfdf` | State/cycle/audio/replay design before code PR |
| Experiment | `exp/android-runtime-douglas` | Douglas, `19233f4`/`6238330` | Separate runtime from packaging; on-device gates |

## Attribution and licensing checklist

| Contributor | Exact author identity to preserve |
|---|---|
| NyperYuhgard | `NyperYuhgard <93950153+NyperYuhgard@users.noreply.github.com>` |
| Kareem Olim | `Kareem Olim <kareemolim@gmail.com>` |
| Martin Penkava | `Martin Penkava <mpenkava1337@gmail.com>` |
| Douglas | `douglasjv <vanderveerdouglas@gmail.com>` |

- Prefer a clean cherry-pick when a whole source commit is acceptable. For a
  subset/reimplementation, add the exact human contributor as co-author and
  cite the original commit/PR. Preserve any existing co-author trailers too.
- The contribution guide says submitted contributions use PSXRecomp's
  PolyForm Noncommercial license. Standalone `mm8` and `r4` currently declare
  no repository license, so do not copy fork-only material from them without
  an upstream PR or explicit permission/licensing confirmation from Douglas.
- Resolve third-party provenance before moving tables, shaders, emulator-derived
  algorithms, or copied constants. Attribution is required even when the final
  implementation is license-compatible.

## Repeat-audit procedure

1. Record the current upstream baseline SHA and local audit time.
2. Resolve each tracked ref's current SHA. If unchanged, update nothing except
   an explicit recheck date when needed.
3. Compare from the fork's true merge base and separately compare its pinned
   framework commit with current master. Do not treat a submodule as local game
   code.
4. Classify deltas as shared correctness, opt-in framework capability,
   game-owned configuration/tooling, generated/artifact content, superseded
   work, or policy/provenance risk.
5. Verify claimed correctness against PSX-SPX, in-tree Beetle, hardware tests,
   or another independent source. A successful game build is integration
   evidence, not hardware proof.
6. Update this ledger before proposing or creating focused branches.
