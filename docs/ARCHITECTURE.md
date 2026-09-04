# Architecture

How PSXRecomp is put together, end to end. For the higher-level "why three ways
of running code" story, read [`EXECUTION_MODEL.md`](EXECUTION_MODEL.md) first;
this doc is the component-level view.

## Two programs: the recompiler and the runtime

PSXRecomp is split into two CMake projects that are built and run separately:

```
  recompiler/           runtime/
  ───────────           ────────
  MIPS  ──▶  C           C + hardware simulation  ──▶  native game binary
  (build-time tool)      (linked with the generated C)
```

- **`recompiler/`** (C++20) is an offline tool. It reads MIPS R3000A machine code
  and emits C source files under `generated/`.
- **`runtime/`** (C99 + C++17) is the engine. It loads the game's assets into an
  emulated PS1 address space, links the generated C in as native functions, and
  simulates the console's hardware around them.

A **game repository** (e.g. TombaRecomp) contains no framework source — it links
this framework in (as a submodule) and provides the game's config, seeds, and
build glue. See [`BUILDING.md`](BUILDING.md#linking-the-framework).

## The recompiler (`recompiler/`)

Two entry points share the same translation core:

- `src/main_bios.cpp` — ingests the flat BIOS ROM (`SCPH1001.BIN`, loaded at
  `0xBFC00000`) and emits `generated/SCPH1001_*.c`.
- `src/main_psx.cpp` — ingests a `PS-X EXE` extracted from a game disc and emits
  `generated/<serial>_*.c`.

The translation pipeline:

| Stage | File | Job |
|---|---|---|
| Decode | `mips_decoder.cpp` (+ vendored `rabbitizer`) | Decode MIPS instructions |
| Control flow | `control_flow.cpp`, `basic_block.cpp` | Build basic blocks / CFG |
| Discovery | `function_discovery.cpp`, `function_analysis.cpp` | Find function entry points (seeded from Ghidra exports) |
| Codegen | `code_generator.cpp`, `full_function_emitter.cpp`, `strict_translator.cpp` | Emit C: one function per guest function, plus a dispatch table |

Output is **two files** per program: a `_full.c` (the function bodies) and a
`_dispatch.c` (the address→function dispatch table). Both are build artifacts and
are **never hand-edited** — if the C is wrong, the fix is in the recompiler.

GTE (the PS1 geometry coprocessor) instructions are emitted inline;
COP0 kernel-mode instructions the BIOS needs are handled in codegen.

## The runtime (`runtime/`)

The runtime is assembled by one CMake helper, `psxrecomp_add_runtime_target()`
(defined in `runtime/runtime.cmake`), which a game's `CMakeLists.txt` calls with
the paths to its generated C. The runtime provides:

- **Memory & address space** (`memory.c`) — the 2 MB main RAM, scratchpad, BIOS
  ROM, and MMIO regions, with the dispatch that routes a guest PC to its native
  function (static, overlay, or interpreter).
- **Hardware simulation via MMIO handlers** — GPU (`gpu.c`, `gpu_sw_renderer.c`,
  `gpu_gl_renderer.c`), DMA (`dma.c`), timers (`timers.c`), CD-ROM
  (`cdrom.c`, `iso_reader.cpp`), MDEC, SIO0 controllers/memory cards
  (`sio.c`, `memcard.c`), SPU (`spu.c`), GTE (`gte.cpp`), and interrupt delivery
  (`interrupts.c`).
- **Host services** — window/input/audio via SDL3 by default (SDL2 is an
  explicit compatibility backend), a cooperative-thread
  scheduler on host fibers (`psx_fiber.c`: Win32 Fibers / POSIX `ucontext`), and
  the optional debug TCP server (`debug_server.c`).

### BIOS: LLE baseline + a swappable HLE tier

The recompiled `SCPH1001.BIN` is the **low-level (LLE) baseline**: it *is* the
kernel, and it is the reference implementation and the correctness oracle. There
are no per-vector HLE shims replacing it.

On top of that, PSXRecomp carries an optional **HLE tier** (`bios_hle`, on by
default for player convenience) that skips the BIOS boot sequence and intercepts
a small set of BIOS services — always falling through to the recompiled BIOS for
anything it doesn't implement. LLE stays fully linked and is what every accuracy
check runs against. Turn it off with `[runtime] bios_hle = false` or
`PSX_BIOS_HLE=0`; with it off the build behaves as pure LLE. (Design notes:
[`docs/internal/HLE_SCHEDULER_CARVEOUT_PLAN.md`](internal/HLE_SCHEDULER_CARVEOUT_PLAN.md).)

Those are **two independent axes**, and the distinction matters because a build
links more than one BIOS ([`BIOS_SELECTION.md`](BIOS_SELECTION.md)):

| axis | what it needs from the image | on retail SCPH-1001 | on bundled OpenBIOS |
|---|---|---|---|
| boot-skip | `shell_entry_phys` — works under pure LLE | yes | yes |
| kernel-call HLE | `deliver_event_ret` — the kernel's own DeliverEvent `$ra` | yes | refused, loudly |

So **"skip the BIOS and go straight to the game" means the same thing on every
BIOS**: the boot-skip is not synthesis, it just returns immediately from the
shell call, so it needs nothing BIOS-specific beyond knowing where the shell is
entered. Whether the *kernel-call* tier is additionally available is a separate,
per-image question, and refusing it must never cancel the boot-skip — deriving
one from the other is the bug fixed in 2026-07. Both axes are decided in one pure
place, `psx_bios_hle_plan()`
([`runtime/include/bios_hle_plan.h`](../runtime/include/bios_hle_plan.h)), which
is unit-tested over the whole matrix.

### Static / overlay / interpreter dispatch

This is the heart of the system and has its own doc,
[`EXECUTION_MODEL.md`](EXECUTION_MODEL.md). In brief: a guest PC resolves to
statically-recompiled native code (BIOS + main EXE), a runtime-compiled **native
overlay** (`overlay_capture.c` → `tools/compile_overlays.py` →
`overlay_loader.c`), or the small **dirty-RAM interpreter**
(`dirty_ram_interp.c`) — in that priority order.

### Overlay compile backend

When an overlay needs compiling, the runtime spawns a C compiler on the
recompiler-emitted C and loads the resulting DLL — it does not JIT in-process.
The backend tier is resolved in `overlay_backend.c` (see `main.cpp` around the
`code_provider` setup):

```
  static  →  gcc  →  tcc
```

- **static** — the overlay was baked into the binary at build time (best case).
- **gcc** — used when a system `gcc` is on `PATH` (the development default).
- **tcc** — TinyCC, the toolchain-free fallback **bundled beside shipped
  executables** in `overlay_toolchain/` (an embedded Python + `tcc.exe`) so
  players never need a compiler installed.

Compiled overlays are stored in a content-addressed cache namespaced by
compiler and target ABI (`<game>/gcc/<arch-abi>/…` vs `<game>/tcc/…`); a `gcc`
shard wins over a `tcc` shard for the same region. See
[`docs/FEATURES.md`](FEATURES.md), [`docs/OVERLAY_CACHE_V2.md`](OVERLAY_CACHE_V2.md),
and [`docs/ASYNC_OVERLAY_COMPILE.md`](ASYNC_OVERLAY_COMPILE.md).

## Renderers

Three GPU backends behind one interface:

- **Software rasterizer** — CPU, most portable, the reference look.
- **OpenGL** — GPU-authoritative VRAM/FBO renderer, the default; moves
  rasterization and supersampling onto the GPU. Falls back to software if GL
  init fails. (See [`docs/internal/GL_RENDERER_HANDOFF.md`](internal/GL_RENDERER_HANDOFF.md).)
- **Vulkan** — experimental. The build option `PSX_ENABLE_VULKAN` defaults **ON**
  (compiled when the SDK tools are present), but it is not the runtime default
  renderer: selecting it also requires the game to offer Vulkan and the user to
  request it, otherwise the runtime falls back to OpenGL.

Widescreen (a genuine wider GTE FOV, not a stretch) is opt-in and gen-time; see
[`WIDESCREEN.md`](WIDESCREEN.md) and
[`docs/internal/NATIVE_WIDE_PLAN.md`](internal/NATIVE_WIDE_PLAN.md).

## The oracle model (how correctness is checked)

PSXRecomp validates itself against **Beetle PSX** (the mednafen-psx libretro
core), run as a **separate process** with an identical TCP JSON debug protocol:

- `psx-runtime` — the recompiled runtime (debug server on port 4370).
- `psx-beetle` — Beetle PSX (debug server on port 4380).

A tool written against one works against the other by switching ports; cross-
checking is done by querying both, never by sharing state in one process. There
is also a first-divergence **co-sim** build that cycle-locksteps the compiled
backend against the interpreter. See
[`docs/internal/COSIM_ORACLE.md`](internal/COSIM_ORACLE.md),
[`docs/config_schema.md`](config_schema.md), and [`TCP_COMMANDS.md`](TCP_COMMANDS.md).

## Configuration

A game is configured by its **game config** (`game.toml`). A **BIOS config**
(`bios/*.toml`) describes a BIOS image's identity and address model.

The two are **not merged.** The BIOS config is consumed only by the recompiler
at build time (`psxrecomp-bios`, and `psxrecomp-game` for the address model);
the shipping runtime never loads it — `runtime/src/main.cpp` contains no call to
`load_bios_config`. `[runtime]` keys therefore take effect only from `game.toml`,
`settings.toml`, the CLI, and the environment. Full schema:
[`docs/config_schema.md`](config_schema.md).

## Where to go next

- [`EXECUTION_MODEL.md`](EXECUTION_MODEL.md) — static/native/interp in depth.
- [`BUILDING.md`](BUILDING.md) — dependencies + build steps.
- [`../CONTRIBUTING.md`](../CONTRIBUTING.md) — dev workflow and rules.
- [`../CLAUDE.md`](../CLAUDE.md) — the exhaustive engineering constitution.
