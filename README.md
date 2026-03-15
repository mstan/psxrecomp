# PSXRecomp

**Generic static recompiler framework for PlayStation 1 games — MIPS R3000A -> C -> native x64**

*Read about how it was built: [I Built a PS1 Static Recompiler With No Prior Experience (and Claude Code)](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/)*

---

## What it is

PSXRecomp translates a PS1 game's MIPS binary into C once, then compiles that C as a native
x64 binary. The result runs directly on PC backed by a thin runtime that provides GPU rendering,
audio, and BIOS services.

This is **not** an emulator. There is no interpreter loop, no cycle counting, no accuracy
trade-offs. Every JAL in the MIPS binary becomes a direct C function call.

PSXRecomp is a **framework** — it provides the recompiler and the runtime. Each game gets its own
repository that pulls in PSXRecomp as a submodule and supplies game-specific configuration.

### Applied example

**[TombaRecomp](https://github.com/mstan/TombaRecomp)** — Tomba! (SCUS-94236, USA) running on PSXRecomp. Boots to gameplay with main menu, FMV, audio, movement, combat, and camera all working.

[![PSXRecomp gameplay demo](https://img.youtube.com/vi/CID9oVhgCyY/maxresdefault.jpg)](https://www.youtube.com/watch?v=CID9oVhgCyY)

---

## Architecture

```
recompiler/              MIPS->C static recompiler
  src/                     Code generator, control flow analysis, MIPS decoder
  lib/                     rabbitizer (MIPS disassembler), ELFIO, fmt

runner/                  Native PC runtime (linked by game repos)
  src/
    launcher.c             Disc picker, disc.cfg persistence, CRC verification
    main_runner.cpp        GLFW window, game loop, frame throttle
    runtime.c              PS1 memory map, BIOS stubs, fiber scheduler
    gpu_interpreter.cpp    GP0/GP1 command parser
    opengl_renderer.cpp    OpenGL 3.3 GPU backend (triangles, textures, VRAM)
    gte.cpp                GTE coprocessor (geometry transform engine)
    spu.cpp                SPU audio (24-voice ADPCM, WinMM output)
    xa_audio.cpp           XA-ADPCM streaming audio from disc
    fmv_player.cpp         STR v2 FMV video decoder
    cdrom_stub.cpp         CD-ROM command emulation
  include/
    game_extras.h          Interface that game repos implement
  lib/                     GLAD (OpenGL loader)
  assets/
    keyconfig.ini          Default key bindings + audio volume
  runner.cmake             Include this from your game's CMakeLists.txt

tools/                   Developer tooling
  pcsx_redux_mcp/          MCP server for PCSX-Redux (reference emulator)
```

The recompiler reads a raw PS1 EXE and emits `<serial>_full.c` (all recompiled game functions)
and `<serial>_dispatch.c` (dynamic call dispatch table). The game repo links these with the
runtime into a native executable. Overlay code (loaded at runtime from disc) runs through a
MIPS interpreter in `runtime.c`.

---

## Making a game project

Each game gets its own repository structured like this:

```
MyGameRecomp/
  psxrecomp/             <- git submodule pointing to this repo
  extras.cpp             <- implements game_extras.h hooks
  CMakeLists.txt         <- pulls in runner.cmake, adds generated code
  annotations/           <- per-function and per-instruction notes (CSV)
  generated/             <- output from the recompiler (gitignored)
  isos/                  <- game disc images (gitignored)
```

### game_extras.h hooks

Your `extras.cpp` must implement these functions:

```c
const char *game_get_name(void);           // window title
uint32_t    game_get_display_entry(void);  // PS1 address of display thread
const char *game_get_exe_filename(void);   // e.g. "SCUS_942.36_no_header"
uint32_t    game_get_expected_crc32(void); // 0 = skip verification
void        game_on_init(void);            // called after runtime init
void        game_on_frame(uint32_t frame); // called every PS1 frame
int         game_handle_arg(const char *key, const char *val);
const char *game_arg_usage(void);
```

### Annotations

Place a CSV at `annotations/<serial>_annotations.csv`:

```
# Format: address, note
0x8001dfd4, entity tick dispatcher — iterates 200 entity slots
0x8002a100, jump table for menu state machine
```

Annotations appear as `/* [NOTE] ... */` in the generated C — both at function start and
inline at instruction addresses.

### Launcher

Double-click the built exe: a file dialog asks for the CUE file on first run, then
saves the path to `disc.cfg` for subsequent launches. The EXE file is found automatically
in the same directory as the CUE. Old-style CLI (`game.exe <exe> <cue>`) still works.

---

## Building

### Requirements

- **OS**: Windows 10+ (64-bit)
- **Compiler**: MinGW-w64 GCC (C++20, C17) via MSYS2
- **Build system**: CMake 3.20+ with Ninja
- **GPU**: OpenGL 3.3+
- **FFmpeg DLLs**: For FMV playback — place `avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`, `swresample-*.dll`, `swscale-*.dll` next to the game exe

### 1. Build the recompiler

```bash
cmake -S recompiler -B build/recompiler -G Ninja
ninja -C build/recompiler
```

### 2. Generate translated C (from your game repo)

```bash
path/to/PSXRecomp.exe isos/SCUS_942.36
```

### 3. Build the game

```bash
cmake -B build -G Ninja
ninja -C build
```

---

## Controls

Default key bindings (configurable via `keyconfig.ini` next to the executable):

| PS1 Button | Key |
|------------|-----|
| D-Pad | Arrow keys |
| Cross (confirm) | S |
| Square | A |
| Triangle | W |
| Circle | D |
| Start | Enter |
| Select | ' (apostrophe) |
| L1 / L2 | 1 / 2 |
| R1 / R2 | 0 / 9 |

---

## AI-assisted development

This project was developed using [Claude Code](https://claude.ai/code) (Anthropic) as
an AI pair programmer, with [Ghidra](https://ghidra-sre.org/) as the decompiler and ground
truth for PS1 code.

### Tooling

- [Claude Code](https://claude.ai/code) — AI coding agent
- [Ghidra](https://ghidra-sre.org/) with the [Ghidra MCP server](https://github.com/LaurieWired/GhidraMCP) — decompiler integration
- `tools/pcsx_redux_mcp/` — MCP server for [PCSX-Redux](https://github.com/grumpycoders/pcsx-redux) (reference PS1 emulator)

---

## Acknowledgments

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** — Inspired by this N64 static recompilation project. PSXRecomp's architecture is directly modeled on N64Recomp's approach. We also use [rabbitizer](https://github.com/Decompollaborate/rabbitizer), the MIPS disassembly library from the N64 decomp community.
- **[PS1Recomp](https://github.com/nihirunoherr/PS1Recomp)** — A PS1 recompiler tool by Wiseguy, referenced during development.

---

## License

PSXRecomp is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE) — free for
non-commercial use with attribution.

Third-party libraries retain their own licenses (see `recompiler/lib/` subdirectories).

Game assets, disc images, and PS1 executables are copyrighted by their respective owners and
are not included in this repository.

---

<sub>[Buy me a coffee](https://ko-fi.com/gamemaster1379)</sub>
