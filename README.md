<p align="center">
  <img src="docs/assets/psxrecomp-logo.png" alt="PSXRecomp" width="640">
</p>

# PSXRecomp

Generic static recompiler framework for PlayStation 1: MIPS R3000A to C to
native x64.

Background on the original prototype:
[I Built a PS1 Static Recompiler With No Prior Experience (and Claude Code)](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/)

[![PSXRecomp demo](https://img.youtube.com/vi/CID9oVhgCyY/maxresdefault.jpg)](https://www.youtube.com/watch?v=CID9oVhgCyY)

## What It Is

PSXRecomp translates PS1 MIPS binaries into C, then compiles that C as a
native executable linked against a PS1 hardware runtime. The v4 architecture
recompiles the real SCPH1001 BIOS and runs it as the kernel. There is no HLE
BIOS layer, no stubs, and no general-purpose interpreter fallback for static
code.

PSXRecomp is a framework. Game-specific projects live in their own
repositories and link this one in as a **git submodule** to build a game binary.
The active end-to-end targets are:

- [TombaRecomp](https://github.com/mstan/TombaRecomp) — *Tomba!*
- [MegaManX6Recomp](https://github.com/mstan/MegaManX6Recomp) — *Mega Man X6*

**New here?** The fastest way in:
[`docs/EXECUTION_MODEL.md`](docs/EXECUTION_MODEL.md) (how a game actually
runs — static / native-overlay / interpreter), then
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
[`docs/BUILDING.md`](docs/BUILDING.md), and
[`CONTRIBUTING.md`](CONTRIBUTING.md).

Adding widescreen, 60 FPS, camera, physics, or other title-specific changes?
Use the typed config features and guarded instruction replacements described in
[`docs/GAME_TWEAKS.md`](docs/GAME_TWEAKS.md) instead of forking psxrecomp.

## Philosophy — toward 100% static recompilation

The goal is simple and absolute: **a PS1 game should run as native code, not be
emulated.** Every MIPS instruction the game executes should ideally have been
translated to C and compiled ahead of time. No interpreter on the hot path, no
HLE shims, no "good enough" approximation of the hardware — the recompiled BIOS
*is* the kernel, and the recompiled game *is* the game.

PS1 games make that goal hard in one specific way: **overlays.** Games stream
code off the disc into RAM at runtime and execute it, then overwrite it with the
next overlay. That code does not exist in the executable at build time, so a
pure ahead-of-time recompiler cannot see it. This is the frontier the project is
working through, and it is why this is an **alpha/beta**: today a *majority* of a
supported game runs as statically recompiled native code, but **not yet 100%.**

How we close the gap, without ever compromising correctness:

1. **Static first.** The main executable and the BIOS are fully recompiled
   ahead of time. This is the bulk of execution and it is always native.
2. **Capture → compile → cache for overlays.** As the game runs, overlays are
   captured the moment they load. Offline, each is recompiled to a native DLL
   keyed by its content, cached, and on later runs loaded and dispatched as
   native code *before* any fallback. Coverage grows as the game is played:
   every overlay someone reaches becomes native for everyone after.
3. **Interpreter failover — only for code that isn't static yet.** A small
   MIPS interpreter runs *runtime-installed* code (overlays/dirty RAM) that
   hasn't been captured-and-compiled. It is a safety net and a coverage feeder,
   never a substitute for recompiling static code, and never on the BIOS/main-EXE
   path.
4. **Precision over recall.** A piece of code we *haven't* compiled safely falls
   back to the interpreter and gets captured for next time — under-coverage
   self-heals. A piece we compile *wrong* would corrupt the machine, so the
   system biases hard toward correctness: native code is only dispatched when its
   source RAM is provably unchanged, and a registration is revoked the instant
   the RAM it was compiled from is overwritten.

Two honest bounds. **The worst case is always performance, never correctness** —
anything not yet native simply runs interpreted, correctly.

**Known corner case — genuinely self-modifying / per-load-relocated code.** Some
code is rewritten or relocated to *different bytes on every load*, so it is not
static by definition and cannot be recompiled ahead of time into a single
correct translation. **This code remains interpreted** — permanently, as far as
the current design is concerned, and that is an accepted, correct outcome (the
interpreter runs it faithfully; only speed is lost). It is a narrow corner, not
a wall. We **may someday aim to cover it** — e.g. by detecting the
relocation/patch pattern and baking it in at compile time (keyed by relocation
parameters), or by compiling at load time — but we make no promise, and the
project is fully correct without it.

The aspiration is **100% static coverage** — every reachable instruction native,
the interpreter idle. The capture-and-recompile loop converges toward it the more
a game is played; this branch is where that machinery is being built.

## Status

Current milestone as of 2026-05-18:

| Subsystem | State |
|---|---|
| BIOS recompilation (`SCPH1001.BIN`) | Boots and hands off to Tomba |
| Game EXE recompilation | Tomba title, OPTIONS, NEW GAME, save/load, and gameplay reached |
| CD-ROM / MDEC / XA | Tomba FMVs stream and play at the game's 15 fps cadence |
| Memory cards | Tomba save and load verified |
| SIO0 controllers | Digital pad polling plus DualShock config replies used by Tomba |
| GPU | Functional for BIOS boot, FMVs, menus, and first gameplay area |
| Interrupts, COP0, timers | Working for current Tomba path |
| Dirty-RAM support | BIOS/game RAM-installed dispatch paths handled |
| Controller input | Keyboard plus SDL/XInput-style controller mapping via `input.ini` |

Known follow-up work:

- The recent Tomba visual burn-down fixed the BIOS PS logo, title/menu glyph
  seams, dialog/pause panel seams, terrain shading, and shaded textured branch
  rendering observed in the first area.
- SPU coverage is partial; reverb, noise, sweep, and accurate SPU IRQ behavior
  are not complete.
- The historical Windows "Not Responding" hang is mitigated but should stay on
  the watch list until longer in-game soak tests are clean.
- Neither game is fully validated end to end yet. Tomba has considerably more
  coverage; Mega Man X6 is playable (stages, controller, and memory-card
  save/load all work). Full playthroughs of both are still unverified.

For the current game milestone, build and run the sibling TombaRecomp project:

```sh
cd ../TombaRecomp
cmake --build build -j16
./build/psx-runtime.exe --game game.toml
```

Running this repository's runtime without `--game` is still useful for
BIOS-only memory card management.

## Release Package

The framework release package is BIOS-only:

1. Download `PSXRecomp-v*-windows-x64.zip` from Releases.
2. Extract it and run `PSXRecomp.exe`.
3. Select your legally obtained `SCPH1001.BIN` BIOS when prompted.

The package does not include a PS1 BIOS, game disc image, generated game code,
or save data. The selected BIOS path is saved next to the executable as
`bios.cfg`; delete that file to pick a different BIOS later.

Game-specific recomp projects, including TombaRecomp, use the same runtime
picker contract but also prompt for a legally obtained game disc image.

## Setup

Builds natively on **Windows (MSVC/MinGW)**, **macOS (Apple Silicon & Intel)**,
and **Linux**. The BIOS thread scheduler uses host fibers — Win32 Fibers on
Windows, `ucontext` on POSIX (`runtime/src/psx_fiber.c`) — so the recompiled
BIOS's cooperative thread switching (the CD-boot handoff in particular) behaves
the same on every platform.

Requirements at a glance (full details, dependency table, and per-platform
prerequisites in [`docs/BUILDING.md`](docs/BUILDING.md)):

- A C/C++ toolchain: MSVC or MinGW/MSYS2 (Windows), Apple Clang (macOS),
  Clang/GCC (Linux). CMake 3.20+; on macOS/Linux also `ninja` and `pkg-config`.
- SDL2 (system / bundled). RmlUi and FreeType come in as **git submodules** —
  clone with `--recurse-submodules`.
- A legally obtained `SCPH1001.BIN` BIOS dump. Not included.
- For game projects, a legally obtained game disc/EXE dump. Not included.

Build the framework (recompiler tool + BIOS-only runtime):

```sh
git clone --recurse-submodules https://github.com/mstan/psxrecomp.git && cd psxrecomp

cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build recompiler/build
cmake -S runtime    -B runtime/build    -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build runtime/build --target psx-runtime
```

On Windows swap `-G Ninja` for your generator if you prefer (e.g.
`-G "Unix Makefiles"`); always keep an explicit `-DCMAKE_BUILD_TYPE` so the
generated C is optimized. Game projects generate their own
`generated/<serial>_*.c` files and link this runtime through CMake — see
[`docs/BUILDING.md`](docs/BUILDING.md#build-and-run-a-game).

## Keyboard Map

| PSX button | Keyboard |
|---|---|
| D-Pad Up / Down / Left / Right | Arrow keys |
| Cross | X |
| Square | Z |
| Circle | S |
| Triangle | A |
| L1 / R1 | Q / W |
| L2 / R2 | E / R |
| L3 / R3 (stick clicks) | T / Y |
| Start | Enter |
| Select | Right Shift |
| Turbo | Tab (hold) |
| Fullscreen | F11 / Alt+Enter / Cmd+F |

## Controller Map

Xbox-style controller defaults are enabled when a controller is connected:

| PSX button | Xbox controller |
|---|---|
| D-Pad Up / Down / Left / Right | D-pad or left stick |
| Cross | A |
| Circle | B |
| Square | X |
| Triangle | Y |
| L1 / R1 | LB / RB |
| L2 / R2 | LT / RT |
| L3 / R3 | Left / right stick click |
| Start | Menu |
| Select | View / Back |

Release builds create/use `input.ini` next to the executable. Edit that file to
change controller device index, deadzone, or button mapping.

## Architecture

The recompiler emits C functions and dispatch tables for BIOS and game code.
The runtime loads the BIOS/game assets into emulated PS1 memory, links the
generated C as native code, and simulates hardware through MMIO handlers for
GPU, DMA, timers, CD-ROM, MDEC, SIO0, memory cards, SPU, GTE, and interrupt
delivery. The recompiled `SCPH1001.BIN` is the low-level (LLE) kernel and the
correctness oracle; an optional HLE tier lays instant boot-skip and a few BIOS
services on top, always falling through to the recompiled BIOS.

Code that can't be seen ahead of time (disc-streamed **overlays**) is captured
and compiled to native code the first time it appears (`static → gcc → tcc`
backend), with a small interpreter as the correctness fallback until it is. Full
story in [`docs/EXECUTION_MODEL.md`](docs/EXECUTION_MODEL.md); component-level
detail in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

See [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`CLAUDE.md`](CLAUDE.md) for the
development rules, and [`docs/internal/`](docs/internal/) for the phased plans
and deep design notes (`PLAN.md`, `FAITHFUL_TIMING_PLAN.md`, …).

## Disc Speed

Per-game `disc_speed` in `game.toml [runtime]` compresses CD-ROM timing:

| Value | Effect |
|---|---|
| `"1x"` | Authentic PSX timing. Default for all games. |
| `"2x"` / `"4x"` | 2× / 4× faster reads and seeks. |
| `"instant"` | Minimum-floor delays. **Known to hang some games** during early initialization; root cause not yet identified. Use `"4x"` instead until resolved. |

FMV playback is always protected: the CD-ROM layer reverts to 1× whenever XA
audio streaming is active, regardless of `disc_speed`. The speed switch fires
only after the BIOS has handed off to the game EXE — boot and the license
screen always run at authentic 1×.

## Help make your game faster — just by playing it

**Why isn't the game already at full speed everywhere?** Most of a game's
code is converted ("recompiled") into a fast native program ahead of time.
But PlayStation games don't keep all of their code on screen at once — they
stream extra chunks of code off the disc as you reach new areas (these
chunks are called *overlays*). We can't convert a chunk we've never seen,
and the only way to see it is for someone to actually visit that area.
Until then, that area's code runs in a slower compatibility mode.

**You can help, just by playing.** While you play, the game quietly notices
which areas are still running in the slow mode, takes a snapshot of them,
and converts them to fast native code in the background — often within a
minute, while you keep playing. The more places you visit, the faster the
game gets. This happens automatically; you don't have to do anything.

**Your discoveries persist for you.** They are saved in a file written next
to the game called `overlay_captures.json`, and your local cache is rebuilt
from it automatically — areas you have visited stay fast on every later
session.

**Please do not post `overlay_captures.json` publicly.** The file contains
verbatim snapshots of the game's code read from your disc, which is
copyrighted material — keep it on your own machine, alongside your disc
image. A metadata-only contribution format (addresses and checksums, no
game code) is planned so discoveries can be shared safely in the future.

## Contributing

Contributions are welcome — AI-assisted or not — as long as they're reviewed,
tested, and keep the core game-agnostic. A few things hold this project together:
the faithful recompiled BIOS is the baseline and oracle, generated code is never
hand-edited (fix the recompiler and regenerate), and a change proves itself
against the Beetle oracle / on screen rather than by assertion. Game-specific work
lives in the game repos, which pin an exact framework commit as a submodule.

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) before opening a PR — it covers the core
rules, how to verify a change, the regression checklist across the known games,
and how a framework fix reaches a game through its pin. Bugs and build problems go
to GitHub issues (include `gcc -v` / OS / generator for build failures); design
discussion happens in the **R.A.I.D.** Discord (invite below).

## License

PolyForm Noncommercial 1.0.0. See `LICENSE`.

The PSX BIOS and game disc images remain copyrighted by their respective
owners. This project distributes no BIOS images, no disc images, and no
game assets — those are always supplied by the user from their own
collection. Release executables (and per-game overlay caches) contain
statically recompiled (machine-translated) builds of the original code,
the same distribution model used by other static recompilation projects
such as N64: Recompiled.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
