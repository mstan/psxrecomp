# PSXRecomp

**Static recompiler for PlayStation 1 games — MIPS R3000A → C → native x64**

> Tech demo — not intended to be a complete or fully playable experience. Tomba! (USA) boots to gameplay with most core systems working.

*Read about how it was built: [I Built a PS1 Static Recompiler With No Prior Experience (and Claude Code)](https://1379.tech/i-built-a-ps1-static-recompiler-with-no-prior-experience-and-claude-code/)*

[![PSXRecomp gameplay demo](https://img.youtube.com/vi/CID9oVhgCyY/maxresdefault.jpg)](https://www.youtube.com/watch?v=CID9oVhgCyY)

---

## What it is

PSXRecomp translates a PS1 game's MIPS binary into C once, then compiles that C as a native
x64 binary. The result runs directly on PC backed by a thin runtime that provides GPU rendering,
audio, and BIOS services.

This is **not** an emulator. There is no interpreter loop, no cycle counting, no accuracy
trade-offs. Every JAL in the MIPS binary becomes a direct C function call.

**Reference game: Tomba! (SCUS-94236, USA)**

The recompiler is the product. Tomba! is the vehicle that drives its development.

---

## Current status

**Working:**
- Main menu, attract FMV, SPU audio
- New Game → gameplay
- Tomba movement, jumping, and camera
- Pig grab and throw
- Mace weapon attack (swing, trail, hit)
- Equipment menu

**Known issues:**
- Wrong sound effects play in some situations; music can sound slightly off
- Some entities don't render fully correctly (e.g. parts of swingable objects)
- Save system is buggy — saves to all slots instead of the selected slot, loading a save goes to a black screen, saves are not persisted to disk
- Many softlock conditions exist (e.g. discovering an objective while already holding the required key item)

This is a tech demo. It is not intended to be exhaustive or fully playable.

---

## How it works

```
recompiler/          MIPS→C static recompiler
  src/                 Code generator, control flow analysis, MIPS decoder
  lib/                 rabbitizer (MIPS disassembler), ELFIO, fmt

runner/              Native PC runtime
  src/
    main_runner.cpp    GLFW window, game loop, frame throttle
    runtime.c          PS1 memory map, BIOS stubs, fiber scheduler
    gpu_interpreter.cpp  GP0/GP1 command parser
    opengl_renderer.cpp  OpenGL 3.3 GPU backend (triangles, textures, VRAM)
    gte.cpp            GTE coprocessor (geometry transform engine)
    spu.cpp            SPU audio (24-voice ADPCM, WinMM output)
    xa_audio.cpp       XA-ADPCM streaming audio from disc
    fmv_player.cpp     STR v2 FMV video decoder
    cdrom_stub.cpp     CD-ROM command emulation
  include/             Headers
  lib/                 GLAD (OpenGL loader)
  assets/
    keyconfig.ini      Default key bindings + audio volume

generated/           Output from recompiler (not checked in)
  tomba_full.c         All recompiled game functions (~200K lines, 2955 functions)
  tomba_dispatch.c     Dynamic call dispatch table

tools/               Developer tooling
  pcsx_redux_mcp/      MCP server integration for PCSX-Redux (reference emulator)
```

The recompiler (`PSXRecomp.exe`) reads the raw PS1 EXE and emits `tomba_full.c`. The runner
links that C with the runtime, renderer, and audio into `PSXRecompGame.exe`. Overlay code
(loaded at runtime from the disc) is handled by a MIPS interpreter in `runtime.c`.

---

## Requirements

- **OS**: Windows 10+ (64-bit)
- **Compiler**: MinGW-w64 GCC (C++20, C17) via MSYS2
- **Build system**: CMake 3.20+ with Ninja
- **GPU**: OpenGL 3.3+ capable graphics card
- **FFmpeg DLLs**: Required for FMV playback — download a Windows build from [ffmpeg.org](https://ffmpeg.org/download.html) and place `avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`, `swresample-*.dll`, and `swscale-*.dll` next to `PSXRecompGame.exe`
- **Game disc**: Tomba! (USA) — `SCUS_942.36` — you must provide your own legally obtained copy

---

## Disc image setup

The runner needs two files from your Tomba! disc image:

1. **The PS1 executable** (headerless) — `SCUS_942.36_no_header`
2. **The disc image** in BIN/CUE format — e.g. `Tomba.bin` + `Tomba.cue`

### Preparing the headerless executable

```bash
# Strip the 2048-byte PS1 EXE header:
dd if=SCUS_942.36 of=SCUS_942.36_no_header bs=2048 skip=1
```

Or use a hex editor to remove the first 2048 bytes.

### Directory layout

Place your files in the `isos/` directory (already in `.gitignore`):

```
psxrecomp/
  isos/
    SCUS_942.36           # Original PS1 executable (for recompiler)
    SCUS_942.36_no_header # Headerless binary (for Ghidra analysis)
    Tomba.bin             # Raw disc image
    Tomba.cue             # CUE sheet
```

---

## Building

### 1. Build the recompiler

```bash
cmake -S recompiler -B build/recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/recompiler
```

### 2. Generate the translated C code

```bash
build/recompiler/PSXRecomp.exe isos/SCUS_942.36
```

This produces `generated/tomba_full.c` and `generated/tomba_dispatch.c`.

### 3. Build the runner

```bash
cmake -S runner -B build/runner -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/runner
```

### 4. Run

```bash
build/runner/PSXRecompGame.exe isos/SCUS_942.36_no_header isos/Tomba.cue
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

This project was developed entirely using [Claude Code](https://claude.ai/code) (Anthropic) as
an AI pair programmer, with [Ghidra](https://ghidra-sre.org/) as the decompiler and ground
truth for PS1 code.

The development loop:

1. Build the runner
2. Run the game, observe the screenshot
3. Compare against the reference (main menu / gameplay)
4. Identify the bug: wrong colors → shader; wrong geometry → GPU decode; crash → Ghidra
5. Ghidra the relevant PS1 function to understand what it actually does
6. Fix `opengl_renderer.cpp`, `gpu_interpreter.cpp`, or `runtime.c`
7. Repeat

`CLAUDE.md` in the repo root documents the exact rules and methodology that govern each
session. It is the system prompt that made this workflow reliable.

### Reproducing the setup

- [Claude Code](https://claude.ai/code) — the AI coding agent
- [Ghidra](https://ghidra-sre.org/) with the [Ghidra MCP server](https://github.com/LaurieWired/GhidraMCP) — decompiler integration
- `tools/pcsx_redux_mcp/` — MCP server for [PCSX-Redux](https://github.com/grumpycoders/pcsx-redux) (reference PS1 emulator, used as ground truth)
- Your own Tomba! (USA) disc

---

## Acknowledgments

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** — Inspired by this N64 static recompilation project. PSXRecomp's architecture is directly modeled on N64Recomp's approach. We also use [rabbitizer](https://github.com/Decompollaborate/rabbitizer), the MIPS disassembly library from the N64 decomp community.
- **[PS1Recomp](https://github.com/nihirunoherr/PS1Recomp)** — A PS1 recompiler tool by Wiseguy, referenced during development.

---

## License

PSXRecomp is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE) — free for
non-commercial use with attribution. For commercial licensing, contact [1379.tech](https://1379.tech).

Third-party libraries retain their own licenses (see `recompiler/lib/` subdirectories).

Game assets, disc images, and PS1 executables are copyrighted by their respective owners and
are not included in this repository.

---

<sub>If this project was useful or interesting to you: [☕ ko-fi.com/gamemaster1379](https://ko-fi.com/gamemaster1379)</sub>
