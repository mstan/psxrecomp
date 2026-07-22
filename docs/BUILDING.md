# Building PSXRecomp

PSXRecomp builds natively on **Windows** (MSVC or MinGW/MSYS2), **macOS** (Apple
Silicon & Intel), and **Linux**. There are two things you can build:

- the **framework** itself (this repo) — a BIOS-only runtime plus the recompiler
  tool; useful for BIOS work and as the thing game repos link against;
- a **game** — done from that game's own repository, which links this framework
  in as a submodule (see [Linking the framework](#linking-the-framework)).

You always supply your own **`SCPH1001.BIN` BIOS** and, for a game, your own
legally-obtained **disc image**. This project ships none of those.

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
| **SDL2** | System. Windows/MSVC: prebuilt pack auto-found at `../sdl2-msvc/SDL2-*`. Windows/MinGW + macOS + Linux: found via pkg-config. | Window, input, GL/Vulkan context, audio, threads |
| **recomp-ui** | **git submodule** `lib/recomp-ui` (Dear ImGui, static) | Pre-boot launcher UI |
| **fmt** 9.1.0 | vendored `recompiler/lib/fmt` | String formatting (runtime uses it header-only) |
| **toml11** | vendored `recompiler/lib/toml11` | Parsing `game.toml` / configs |
| **ELFIO** | vendored `recompiler/lib/ELFIO` | ELF parsing (recompiler only) |
| **rabbitizer** | vendored `recompiler/lib/rabbitizer` | MIPS instruction decoding (recompiler only) |
| **TinyCC (TCC) 0.9.27** | Not in this repo — downloaded at release-packaging time and bundled beside the game exe in `overlay_toolchain/`. | Toolchain-free overlay compilation for players (run as a subprocess) |
| **Python 3** | System (development) or an embedded copy bundled in releases | Runs `tools/compile_overlays.py` in the overlay pipeline |
| **OpenGL** | System (`opengl32` on Windows; `find_package(OpenGL)` elsewhere) | The GL renderer |
| **Vulkan** | Headers only, optional, **off by default** (`PSX_ENABLE_VULKAN=OFF`); loaded dynamically via SDL. Shader compilation needs `glslc` from the Vulkan SDK. | The experimental Vulkan renderer |

Developers building overlays locally just need `gcc` on `PATH` (the `gcc` tier);
the bundled `tcc` matters only for end-user release packages.

### Get the source (with submodules)

`lib/recomp-ui` is a submodule — clone recursively (or init after the fact):

```sh
git clone --recurse-submodules https://github.com/mstan/psxrecomp.git
# or, in an existing clone:
git submodule update --init --recursive
```

Pre-boot launcher: Dear ImGui via `lib/recomp-ui` when `PSX_LAUNCHER=ON`
(default). Use `-DPSX_LAUNCHER=OFF` to boot straight in.

## Per-platform prerequisites

**Windows (MSYS2/MinGW — recommended for release parity):**
```sh
# In an MSYS2 MinGW64 shell:
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-ccache
```

**macOS:**
```sh
brew install sdl2 pkg-config ninja cmake
```

**Linux (Debian/Ubuntu):**
```sh
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev
```

## Build the framework

Two CMake trees: the recompiler (a tool) and the runtime (the engine).

```sh
# 1. Recompiler tool → produces psxrecomp-bios and psxrecomp-game
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build

# 2. (Optional) regenerate the BIOS C from your SCPH1001.BIN
#    Requires bios/SCPH1001.BIN to be present.
bash tools/regen_bios.sh

# 3. Runtime → produces psx-runtime (BIOS-only for this repo)
cmake -S runtime -B runtime/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/build --target psx-runtime
```

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
| `PSX_STATIC_RUNTIME` | ON for MinGW Release | Self-contained exe (statically links SDL2 + libgcc/libstdc++) |
| `PSX_LAUNCHER` | ON | Build the Dear ImGui (recomp-ui) launcher |
| `PSX_ENABLE_VULKAN` | OFF | Build the experimental Vulkan renderer |
| `PSX_BUILD_COSIM` | OFF | Build the first-divergence co-sim oracle target |

**Profile-guided optimization (game repos):** MotK and other titles may expose
`-DPSX_PGO=generate|use` in their own `CMakeLists.txt` for host-bound scenes
(e.g. intro FMV). See **[PGO.md](PGO.md)** for concepts, MotK’s
`scripts/pgo_motk_intro.sh`, and when to retrain.

## Build and run a game

From the **game's** repository (not this one). Each game repo links this
framework in and provides its own `game.toml`, seeds, and generated C. Example
for TombaRecomp:

```sh
# Extract the game's PS-X EXE from your disc (helper included in the game repo):
python3 tools/extract_psx_exe.py tomba/tomba.bin SCUS_942.36 tomba/SCUS_942.36

# Regenerate the game's C from the disc/EXE (game repos invoke the recompiler
# binary directly with their config):
../psxrecomp/recompiler/build/psxrecomp-game --config game.toml

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

**`SDL2 MSVC dev package not found`.** The MSVC build expects the prebuilt SDL2
pack beside the repo at `../sdl2-msvc/SDL2-*`. Use the MSYS2/MinGW toolchain
(which finds SDL2 via pkg-config) or place the pack there.

**Overlays never compile / stay slow.** In development you need `gcc` on `PATH`
for the `gcc` tier; otherwise areas stay in the interpreter. See
[`EXECUTION_MODEL.md`](EXECUTION_MODEL.md).
