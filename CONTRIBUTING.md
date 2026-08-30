# Contributing to PSXRecomp

Thanks for your interest! PSXRecomp is a static recompiler, not an emulator, and
it holds itself to a high correctness bar. This guide covers how the project is
organized, the rules that keep it faithful, and how to get a change merged.
Contributions are welcome — AI-assisted or not — as long as they are reviewed,
tested, and keep the core game-agnostic.

New to the codebase? Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
[`docs/EXECUTION_MODEL.md`](docs/EXECUTION_MODEL.md) first, then
[`docs/BUILDING.md`](docs/BUILDING.md) to get a build going.

## Repository layout

- **`mstan/psxrecomp`** (this repo) — the framework: the recompiler tool
  (`recompiler/`), the runtime engine (`runtime/`), shared tools, and docs.
  Framework changes go here.
- **Per-game repos** (`mstan/TombaRecomp`, `mstan/MegaManX6Recomp`,
  `mstan/ApeEscapeRecomp`, …) — one per title. Each keeps **game code at the
  repo root** and pins this framework (and usually `recomp-ui`) as **root-level
  submodules** (`psxrecomp/`, `recomp-ui/`). Game-specific work goes in the game
  repo; nothing game-specific belongs in the framework.

Each game repo pins an **exact framework commit** — the git submodule pointer at
`psxrecomp/` (some older game repos also carry a human-readable `psxrecomp-v4.pin`
record; the submodule pointer is the source of truth). The framework evolves on
its own cadence; a game only moves to a newer framework when someone deliberately
bumps that submodule pointer.

**Starting a new title or shipping a setup-host zip?** Follow
[`docs/GAME_PROJECT_SETUP.md`](docs/GAME_PROJECT_SETUP.md) (layout, CI template
under `docs/ci/templates/setup-release.yml`, release checklist). Also see
[Linking the framework](docs/BUILDING.md#linking-the-framework) and
[Framework changes and the pin](#framework-changes-and-the-pin) below.

## The core rules

These are non-negotiable because violating them is how past efforts turned into
un-debuggable emulators. The full engineering constitution is
[`CLAUDE.md`](CLAUDE.md); the debugging philosophy is
[`PRINCIPLES.md`](PRINCIPLES.md). The short version:

1. **Build the faithful core; no per-game hacks in the foundation.** The correct
   fix is the general, hardware-accurate one, not a surgical workaround for one
   title. No title checks in the runtime, no magic PC addresses, no spoofed
   return values for one game. (Per-game *enhancements* — widescreen, faster
   loads — are legitimate once the faithful core is proven; see
   [`ENHANCEMENTS.md`](ENHANCEMENTS.md).)
2. **No stubs.** A function is fully implemented or it fails loudly. No
   `return 0;` placeholders, no `// TODO` behavior, no hand-delivered fake
   events. If execution reaches code we can't handle, we stop and fix
   discovery/codegen — we don't fake the result.
3. **Never hand-edit generated code.** Files under `generated/` and the
   recompiler's output are build artifacts. If the emitted C is wrong, fix the
   recompiler (`recompiler/src/`), then regenerate. (The one exception: a PR
   whose explicit purpose is to *inspect or prove* a generator bug.)
4. **LLE is the baseline; HLE is a validated subsystem replacement, never a
   starting point.** The recompiled BIOS is the reference and oracle. An HLE tier
   is allowed only where the LLE path has a genuine landmine, must be general
   (every game), must operate on the real guest structures, and must be
   continuously checked against the Beetle oracle — never a "produce the answer
   the BIOS would have" shim.
5. **No `printf`/log-file debugging.** Runtime inspection goes through the TCP
   debug server ([`TCP_COMMANDS.md`](TCP_COMMANDS.md)) and always-on ring
   buffers, not `fprintf`.
6. **Fix broken tooling immediately** — don't route around it with indirect
   evidence, and don't infer correctness from two implementations sharing a bug.

## AI-assisted and human contributions

Both are welcome. Using AI does not reduce your responsibility for the result:
review, understand, and test any code before submitting it, and don't submit a
large AI-generated rewrite you can't explain or validate. A PR — however it was
written — is likely to be rejected if it:

- Introduces broad, unfocused changes, or combines unrelated refactors with
  behavioral fixes.
- Hardcodes game-specific behavior into the framework, or adds a title check to
  the core runtime.
- Modifies generated output instead of fixing the generator/source.
- Introduces regressions in known-working games.
- Contains code the contributor cannot reasonably explain, or whose verification
  cannot be stated.

## Game-specific work belongs in game repos

The framework stays game-agnostic. Per-game patches, launcher UI, assets, memory
maps, metadata, config, and workarounds live in the appropriate game repo, not
here. If a behavior differs by game, disc, executable, region, BIOS, or runtime
configuration, it should be **parameterized** (config option, per-game manifest
value, a parameter passed in from the game repo) or **modeled** as the real
hardware/runtime behavior — never special-cased in the core.

Known game repos:

- [TombaRecomp](https://github.com/mstan/TombaRecomp) — *Tomba!*
- [MegaManX6Recomp](https://github.com/mstan/MegaManX6Recomp) — *Mega Man X6*
- [ApeEscapeRecomp](https://github.com/mstan/ApeEscapeRecomp) — *Ape Escape*

Maintainers record evaluated external branches, game repos, exact revisions,
and reusable upstream candidates in the
[`ecosystem watch`](docs/ecosystem-watch.md). Check it before porting work from
an older framework fork.

### Framework changes and the pin

Because each game pins an exact framework commit (its `psxrecomp/` submodule
pointer), a framework fix does **not** reach a game until that game bumps that
pointer. When your framework change is meant to fix or improve a specific title:

- **Link the game repo** in your PR (and a matching game-repo branch/commit/PR if
  you have one). This lets maintainers build and test the fix in its real
  integration environment.
- **Build the game against your framework branch** to prove it. The game's
  `CMakeLists.txt` points `PSXRECOMP_ROOT` at the submodule; you can build
  against a working checkout via the junction/symlink dev setup, or override
  `PSXRECOMP_ROOT` at configure time to point at your branch — no submodule
  surgery required. See [`docs/BUILDING.md`](docs/BUILDING.md#linking-the-framework).
- **Say whether a pin bump is required to consume the fix**, and whether it
  requires a **regen** (codegen/emitter changes do; runtime-only changes usually
  don't — note which). Bumping the submodule pointer is a deliberate, reviewable
  change made in the game repo, not something a framework PR does on its own.

## Verifying a change

Correctness is demonstrated, not asserted:

- **Oracle-check against Beetle.** Run `psx-beetle` alongside `psx-runtime` and
  compare via the shared TCP protocol; for timing/codegen, use the co-sim build.
  See [`docs/internal/COSIM_ORACLE.md`](docs/internal/COSIM_ORACLE.md).
- **Verify visually, end to end.** A milestone is "the pixels appear on screen"
  — take a screenshot, don't trust a counter or a ring-buffer flag alone.
- **A decoder / codegen change requires a playthrough check**, not just a clean
  build: cosim/lockstep first, then run the affected area.

### Automated tests — run these first

```sh
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build
cd recompiler/build && ctest --output-on-failure
```

38 tests, under five seconds, no BIOS dump or disc image required. See
[`docs/TESTING.md`](docs/TESTING.md) for running individual tests, what the
suite covers, and the three known-failing tests that are deliberately not
registered.

If you add a test, register it with `ctest` in the same commit. An unregistered
test cannot fail, and a test that cannot fail is not a test.

### Regression checklist

The automated suite does not cover whether a game still runs; only playing one
does. A framework change should not break a game that worked before it. When
practical, build against the known-working game repos above and confirm, for
each:

- The game **boots**.
- The **attract demo** plays without issue.
- A save file can be **created**.
- A save file can be **loaded**.
- **Gameplay is reachable** after saving/loading.

Perfect compatibility across every game isn't required for every PR, but the PR
must clearly document **what was tested and on which games**. Useful notes: host
OS, compiler/toolchain, game repo + version/region, whether it boots / reaches
gameplay / save-load works, and whether memory cards, audio, video, input, CD
loading, or overlays were touched. Call out any known regression or uncertain
area explicitly — a documented uncertainty is fine; a silent one is not.

## No copyrighted assets

Do not submit copyrighted binary content to this repository. This includes, but
is not limited to:

- PlayStation BIOS files
- Game ROMs, ISOs, BIN/CUE files, or disc images
- Extracted game assets — music, sound, textures, models, video, fonts, artwork
- Proprietary SDK files
- Decompiled copyrighted game source

Small factual references — addresses, symbols, hashes, structure descriptions,
compatibility notes — are acceptable when used for tooling, debugging, or
documentation. The local artifacts above are gitignored in every repo for exactly
this reason (copyright + size); keep them out of commits.

## Adding a new game

At a high level: create a game repo from the template, add this framework as the
`psxrecomp/` submodule, seed function starts (typically from a Ghidra export in
`seeds/`), write `game.toml`, generate the game C with
`psxrecomp-game --config game.toml`, then build and bring the title up boot →
menu → gameplay, oracle-checking as you go. The existing game repos
(TombaRecomp, MegaManX6Recomp) are the working references.

## Pull requests

Keep PRs focused — one thing: one bug, one feature, one diagnostic, one isolated
refactor, or docs for one topic. Avoid combining unrelated refactors, formatting
churn, and behavioral fixes in the same PR.

- Branch off `master`; keep framework changes and game changes in their
  respective repos.
- **Do not commit** local artifacts: BIOS images, disc images, generated C
  (`generated/`), memory cards, Ghidra databases, build outputs, or overlay
  capture files.
- Explain *what hardware behavior* the change matches and *how you verified it*
  (oracle diff, screenshot, playthrough). Changes that can't state their
  verification are hard to accept.
- Keep the fix minimal and at the right layer (recompiler vs runtime vs game
  config). If you find yourself editing generated code or adding a stub, step
  back — the real fix is upstream of that.

When opening a PR, please include: what changed and why; whether it's
game-agnostic or needs per-game configuration; what games/tests were run; any
risks or known limitations; links to affected game repos/branches/commits when
the change targets a specific game; and screenshots, logs, or traces when useful.

## Bug reports

Good bug reports are extremely helpful. Please include:

- The game repo being used, and version/region if known.
- Your OS and build type/configuration.
- Steps to reproduce, expected behavior, and actual behavior.
- Logs, traces, screenshots, or video if available.
- Whether it happens in **LLE (recompiled BIOS)**, **HLE**, and/or the
  **dirty-RAM interpreter** path — and in the software vs OpenGL vs Vulkan
  renderer, if relevant.
- Whether it's a **regression** from a previous commit, and the last good one if
  you know it.

For gameplay bugs, include the nearest save file or a reproduction path when
possible. For build failures, include `gcc -v` / your toolchain / the generator.

## Dependencies and portability

Avoid unnecessary dependencies. When adding one, explain why it's needed and how
it affects portability, build complexity, licensing, or distribution. The project
should stay practical to build and run for contributors.

## Style and maintainability

Prefer clear, boring, maintainable code that reads like the code around it: match
the surrounding style and naming, keep changes localized, and comment non-obvious
hardware behavior or timing assumptions. Prefer diagnostics that can be compiled
out when not needed. Avoid cleverness unless it's clearly justified.

## Licensing

By contributing, you agree your contribution is licensed under the same license as
the project. Do not submit code you don't have the right to contribute, and do not
copy from incompatible licenses or proprietary sources.

### Upstream-derived work and credit

When a change adapts code or behavior from another branch or repository, add a
short record under `docs/internal/upstream/`. The record must include:

- clickable links to the source repository or PR and the exact source commit;
- the original author identity and how commit/co-author credit is preserved;
- the behavior or files actually adapted;
- related source material deliberately excluded from the focused change; and
- the validation performed after rebuilding the change on current `master`.

Prefer preserving the source commit's author when a clean cherry-pick is
appropriate. For a reconstructed or partial port, add the human contributor's
exact `Co-authored-by` trailer and cite the source in the commit or PR body.
Attribution does not replace license compatibility or permission.

## Be respectful

Contributors use different tools, workflows, and levels of AI assistance. Review
the contribution, not the person. Technical criticism is welcome; personal attacks
are not.

## Questions / community

Development happens with the **R.A.I.D. (Retro AI Development)** community —
Discord invite in the [README](README.md). Open a GitHub issue for bugs and
build problems (include `gcc -v` / OS / generator for build failures).
