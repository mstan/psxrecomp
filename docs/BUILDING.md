# Building PSXRecomp

PSXRecomp builds natively on **Windows** (MSVC or MinGW/MSYS2), **macOS** (Apple
Silicon & Intel), and **Linux**. There are two things you can build:

- the **framework** itself (this repo) — a BIOS-only runtime plus the recompiler
  tool; useful for BIOS work and as the thing game repos link against;
- a **game** — done from that game's own repository, which links this framework
  in as a submodule (see [Linking the framework](#linking-the-framework)).

For the retail **`SCPH1001.BIN` BIOS** you supply your own dump; the
MIT-licensed **OpenBIOS** alternative is bundled (`bios/openbios.bin`, see
[Regenerating BIOS backends](#regenerating-bios-backends)). For a
game you always supply your own legally-obtained **disc image** — this
project ships no game data.

## Toolchain requirements

- A C/C++ toolchain:
  - **Windows:** MSYS2 MinGW-w64 (`mingw-w64-x86_64` toolchain) *or* MSVC.
  - **macOS:** Apple Clang (Xcode command-line tools).
  - **Linux:** GCC or Clang.
- **CMake ≥ 3.20**. On macOS/Linux also **Ninja** and **pkg-config**.
- Language standards used: recompiler is **C++20**; runtime is **C99 + C++17**.
- `ccache` is auto-detected and used if present (optional, speeds rebuilds).

## Dependencies

| Dependency | How it's provided | Used for |
|---|---|---|
| **SDL3 3.4+** | Preferred system package; otherwise SDL 3.4.10 is fetched from the official release archive with a pinned SHA-256. | Window, input, GL/Vulkan context, audio, threads |
| **SDL2** | Optional explicit fallback (`-DPSX_SDL_BACKEND=SDL2`). Windows/MSVC uses `../sdl2-msvc/SDL2-*`; MinGW/macOS/Linux use pkg-config. | Compatibility and A/B testing |
| **fmt** 9.1.0 | vendored `recompiler/lib/fmt` | String formatting (runtime uses it header-only) |
| **toml11** | vendored `recompiler/lib/toml11` | Parsing `game.toml` / configs |
| **ELFIO** | vendored `recompiler/lib/ELFIO` | ELF parsing (recompiler only) |
| **rabbitizer** | vendored `recompiler/lib/rabbitizer` | MIPS instruction decoding (recompiler only) |
| **TinyCC (TCC) 0.9.27** | Not in this repo — downloaded at release-packaging time and bundled beside the game exe in `overlay_toolchain/`. | Toolchain-free overlay compilation for players (run as a subprocess) |
| **Python 3** | System (development) or an embedded copy bundled in releases | Runs `tools/compile_overlays.py` in the overlay pipeline |
| **OpenGL** | System (`opengl32` on Windows; `find_package(OpenGL)` elsewhere) | The GL renderer |
| **Vulkan** | Headers only, **on by default** (`PSX_ENABLE_VULKAN=ON`) — built when the SDK tools are available, otherwise skipped; loaded dynamically via SDL. Shader compilation needs `glslc` from the Vulkan SDK. Pass `-DPSX_ENABLE_VULKAN=OFF` to exclude it. | The experimental Vulkan renderer |

Developers building overlays locally just need `gcc` on `PATH` (the `gcc` tier);
the bundled `tcc` matters only for end-user release packages.

### Get the source

This repository has two submodules, **neither of which is required** for the
recompiler and runtime builds described below:

| Submodule | Needed when |
|---|---|
| `recomp-ui` | `-DPSX_RECOMP_UI=ON` (the launcher UI). Configure **fails** with a `FATAL_ERROR` if this is ON and the submodule is absent. |
| `lib/recomp-net` | `-DPSX_NETPLAY=ON` (netplay). |

A plain clone is enough to build the recompiler and the runtime:

```sh
git clone https://github.com/mstan/psxrecomp.git
```

Add `--recurse-submodules` if you intend to build the launcher or netplay:

```sh
git clone --recurse-submodules https://github.com/mstan/psxrecomp.git
```

## Per-platform prerequisites

**Windows (MSYS2/MinGW — recommended for release parity):**
```sh
# In an MSYS2 MinGW64 shell:
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja mingw-w64-x86_64-ccache
```

**macOS:**
```sh
brew install ninja cmake
```

**Linux (Debian/Ubuntu):**
```sh
sudo apt install build-essential cmake ninja-build
```

These are the SDL3-default prerequisites. For the SDL2 fallback, additionally
install `mingw-w64-x86_64-SDL2`, `sdl2`, or `libsdl2-dev` respectively, plus
`pkg-config` outside MSVC.

## Build the framework

Two CMake trees: the recompiler (a tool) and the runtime (the engine).

```sh
# 1. Recompiler tool → produces psxrecomp-bios and psxrecomp-game
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build

# 2. REQUIRED before the first runtime build: generate a BIOS backend.
#    Step 3 configures psx-runtime from generated/OpenBIOS_full.c. If generated/
#    is empty, CMake does NOT fall back -- it fails at configure time with
#    "Cannot find source file: .../generated/OpenBIOS_full.c" followed by
#    "No SOURCES given to target: psx-runtime". Re-run this step whenever the
#    recompiler emitter changes; a stale generated/ raises a fingerprint-mismatch
#    warning from runtime.cmake.
#
#    OpenBIOS can always be regenerated from the tracked image. Regenerating
#    the retail backend requires your own bios/SCPH1001.BIN dump and is genuinely
#    optional -- OpenBIOS alone is enough to build and boot.
bash tools/regen_bios.sh --config bios/OpenBIOS.toml
bash tools/regen_bios.sh --config bios/SCPH1001.toml   # optional, needs your own dump

# 3. Runtime → produces psx-runtime (BIOS-only for this repo)
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPSX_RECOMP_UI=OFF
cmake --build runtime/build --target psx-runtime
```

On Linux/macOS, `tools/setup_dev.sh` performs the same source-checkout setup:

```sh
sh tools/setup_dev.sh
```

It checks the native toolchain, builds the CLI and recompiler tools, refreshes
generated BIOS C when `bios/SCPH1001.BIN` is present, and builds the BIOS-only
runtime when BIOS/generated sources are available. It does not create per-game
runtime targets; use the CLI generator for game projects.

### Check the build

After step 1 above — no BIOS or disc needed — verify the tree is sane:

```sh
cd recompiler/build && ctest --output-on-failure
```

38 tests, under five seconds. See [`TESTING.md`](TESTING.md).

On Windows with MSVC or plain MinGW makefiles, swap `-G Ninja` for your generator
(e.g. `-G "Unix Makefiles"`); everything else is identical.

> **Build type matters.** With no `-DCMAKE_BUILD_TYPE`, the framework defaults to
> **Release** (optimized). The huge generated C compiles unusably slowly at `-O0`,
> so never build it debug-by-accident. Use `-DCMAKE_BUILD_TYPE=RelWithDebInfo`
> when you need symbols. Release turns the debug TCP server **off**; RelWithDebInfo
> and Debug turn it **on** (`PSX_DEBUG_TOOLS`).

### Useful CMake options

| Option | Default | Effect |
|---|---|---|
| `PSX_DEBUG_TOOLS` | ON for Debug/RelWithDebInfo, OFF for Release | TCP debug server + heartbeat + per-block recording |
| `PSX_SDL_BACKEND` | `SDL3` | Host backend. Set `SDL2` explicitly for compatibility or A/B testing. |
| `PSX_SDL3_FETCH` | ON | Fetch the pinned SDL3 release when a compatible system SDL3 package is unavailable. Set OFF for offline/system-only builds. |
| `PSX_STATIC_RUNTIME` | ON for MinGW Release | Self-contained exe (statically links SDL + libgcc/libstdc++) |
| `PSX_RECOMP_UI` | ON | Wire a downstream game's pinned recomp-ui launcher; set OFF for headless/generated builds |
| `PSX_ENABLE_VULKAN` | **ON** | Build the experimental Vulkan renderer when the SDK tools are present (skipped if not). Pass `OFF` to exclude it outright. |
| `PSX_NETPLAY` | OFF | Link recomp-net + lobby; advertise full netplay UI (multiplayer titles) |
| `PSX_SETUP_WIZARD` | OFF | Advertise first-run setup wizard + Generate & rebuild in recomp-ui |

See [SDL backends](SDL_BACKENDS.md) for the fallback command and the initial
SDL3/SDL2 game A/B results.

## Build and run a game

From the **game's** repository (not this one). Each game repo links this
framework in and provides its own `game.toml`, seeds, and generated C. Example
for TombaRecomp:

```sh
# Extract the game's PS-X EXE from your disc (helper included in the game repo):
python3 tools/extract_psx_exe.py tomba/tomba.bin SCUS_942.36 tomba/SCUS_942.36

# Regenerate the game's C from the disc/EXE. The framework is a submodule at
# psxrecomp/ inside the game repo, so build its recompiler once, then run it.
# (Game repos also ship tools/regen.sh (macOS/Linux) / tools/regen.ps1 (Windows)
# that wrap this command.)
cmake -S psxrecomp/recompiler -B psxrecomp/recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build psxrecomp/recompiler/build
psxrecomp/recompiler/build/psxrecomp-game --config game.toml

# Configure + build the game runtime:
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target psx-runtime
./build/psx-runtime --game game.toml --disc tomba/tomba.cue
```

## Linking the framework

Game repositories do **not** vendor the framework. They reference it as a git
**submodule** at path `psxrecomp/`, which the game's `CMakeLists.txt` points
`PSXRECOMP_ROOT` at:

```cmake
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH "Path to psxrecomp")
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
```

A fresh clone of a game repo gets the framework automatically:

```sh
git clone --recurse-submodules https://github.com/mstan/TombaRecomp.git
```

**Local dev tip — share one framework checkout across games.** If you work on
several game repos plus the framework at once, you don't want N multi-GB copies.
Replace each game's `psxrecomp/` submodule working directory with a **junction /
symlink** to a single shared framework checkout:

```sh
# Windows (junction):
cmd /c mklink /J psxrecomp F:\path\to\shared\psxrecomp
# macOS / Linux (symlink):
ln -s /path/to/shared/psxrecomp psxrecomp
```

Git treats the link's target as the submodule working tree, so
`git submodule status` stays clean **as long as the shared checkout is at the
commit the game pins**. Bump a game to a newer framework by checking out the new
commit in the shared repo and committing the updated submodule pointer in the
game repo. This is the setup the game repos are configured for.

## Troubleshooting

**MinGW: `Error: too many sections` / a `*.c.o` (or `.obj`) fails to assemble.**
Windows COFF object files have a 32,768-section limit; very large translation
units (the generated game C, and `debug_server.c` in debug-info builds) can
exceed it on older binutils. The fix is to assemble those units as *big objects*:
add `-Wa,-mbig-obj` to their compile options (recent binutils, ≥ ~2.40, handle
these files without it). If you hit this on a framework source file, please open
an issue with your `gcc -v` / `as --version` — the build should apply the flag
for you.

**`SDL3 3.4+ was not found`.** The default build normally downloads the pinned
release. Check network access, install a system SDL3 package and provide
`SDL3_DIR`, or re-enable `-DPSX_SDL3_FETCH=ON`.

**`SDL2 MSVC dev package not found`.** This applies only when
`-DPSX_SDL_BACKEND=SDL2` is selected. Place the prebuilt SDL2 pack beside the
repo at `../sdl2-msvc/SDL2-*`, or use the MSYS2/MinGW toolchain with SDL2
available through pkg-config.

**Overlays never compile / stay slow.** In development you need `gcc` on `PATH`
for the `gcc` tier; otherwise areas stay in the interpreter. See
[`EXECUTION_MODEL.md`](EXECUTION_MODEL.md).

**`ninja: error: loading 'build.ninja': GetLastError() = 2`, or CMake's
`Error: could not load cache`.** Both mean the same thing: you ran a *build* in
a directory that was never successfully *configured*. `CMakeCache.txt` is
written before the generate step, so a configure that ends in "Configuring
incomplete, errors occurred!" leaves a cache behind but no `build.ninja` /
`Makefile` — and the build then fails on the missing generator file rather than
on the original configure error. Fix the configure error, re-run the same
`cmake -S ... -B ...` and let it finish; if it keeps failing, delete the build
directory so a stale cache cannot poison the retry. `tools/regen_bios.sh`
diagnoses this for the recompiler build directory rather than passing it to
CMake.

**`No recompiled BIOS backend available` at configure time.** Expected on a
fresh clone: the recompiled BIOS C is build output and is not tracked. Run
step 2 of [Build the framework](#build-the-framework)
(`bash tools/regen_bios.sh --config bios/OpenBIOS.toml`) before configuring the
runtime or a game. The runtime reads those files from
`<framework>/generated/<stem>_full.c` and `<stem>_dispatch.c`; that location is
fixed, and the emitter writes there because `out_dir = "generated"` in
`bios/<stem>.toml`.

**`regen_bios: no usable recompiler build dir found`.** The script builds the
BIOS emitter but never configures it, so step 1 of
[Build the framework](#build-the-framework) has to have run first.
`PSXRECOMP_BIOS_BUILD` overrides which directory it uses, and is resolved
**relative to the framework root**, not your shell's working directory — from a
game project that vendors the framework, that is `recompiler/build`, never
`psxrecomp/recompiler/build`.

## Regenerating BIOS backends

Every normal runtime links both statically recompiled BIOS backends. OpenBIOS
symbols and retail SCPH-1001 symbols are namespaced so they coexist in one
executable; the active backend is selected at launch:

```bash
# OpenBIOS (tracked and redistributable)
tools/regen_bios.sh --config bios/OpenBIOS.toml

# Retail SCPH-1001 (requires your own local dump)
tools/regen_bios.sh --config bios/SCPH1001.toml
```

`PSXRECOMP_BIOS_STEMS` defaults to `OpenBIOS;SCPH1001`. The runtime build
automatically stages `bios/openbios.bin` and `bios/OpenBIOS.LICENSE` beside
every native executable. A release packager must copy that `bios/` directory
unchanged.

At runtime, no explicit player choice means OpenBIOS. A retail BIOS selected
with the launcher, `settings.toml`, or `--bios` wins after its exact identity is
verified. `[runtime] openbios = false` is reserved for a title with a verified
OpenBIOS incompatibility. See [`BIOS_SELECTION.md`](BIOS_SELECTION.md).

OpenBIOS seeds come from its ELF symbol tables (no Ghidra pass needed):
see the pin + regeneration recipe in `bios/OpenBIOS.toml`.
