# PSXRecomp v4 — Plan for Static Recompilation of the PlayStation BIOS

> **Audience for this document:** an outside reviewer (ChatGPT or a
> human engineer) who has not seen any of the prior versions. The goal
> is for this document to be self-contained: by the end you should
> understand what we are building, why three previous attempts failed,
> what we are deliberately doing differently, what the hard problems
> are, and where we want outside input on architectural decisions.

---

## TL;DR

We are building **PSXRecomp v4**, a static MIPS-to-C recompiler that
takes the PlayStation 1 BIOS ROM (`SCPH1001.BIN`, 512 KB of MIPS R3000A
machine code) and emits a single `SCPH1001_full.c` file containing every
BIOS function as a native C function. That generated C is then linked
into a small SDL2-based runtime which simulates the PS1 hardware
peripherals (GPU, DMA, timers, interrupts, controllers, etc.) over MMIO.
The recompiled BIOS calls into those simulations the same way real BIOS
code touches `0x1F801xxx` MMIO on real hardware. There is **no MIPS
interpreter** in the BIOS execution path. There are **no HLE shims** that
fake the output of BIOS functions. The BIOS runs as native compiled C.

Once the BIOS boots cleanly through the Sony logo and into the BIOS
shell (Phases 1-4), we add a second recompilation entry point for PS1
game EXEs (Phase 5), starting with Tomba! as the first end-to-end
target including save/load via memory card (Phase 6).

This is the project's third clean restart. We are doing it because the
prior three attempts (v1, v2, v3) all collapsed into the same shape:
**interpreter + HLE emulator pretending to be a recompiler**. v4 forbids
that shape architecturally so it cannot be reached again under
schedule pressure.

---

## Background: three failed attempts

### v1 — `F:/Projects/psxrecomp/` — 126 sessions

The original attempt. Worked through Tomba! to a playable state with
working renderer and GTE. **The BIOS was almost entirely stubbed.**
Save/load was broken because the kernel data structures (PCB, TCB,
EvCB) the game relied on were never initialized properly — there was no
real BIOS init pass; the runner just faked the values it thought the
BIOS would have written. Game ran, save/load fundamentally did not.
Project archived after 126 sessions of work without resolving the save
issue.

### v2 — `F:/Projects/psxrecomp-v2/` — Severed Chains attempt

A pivot to a different game (Severed Chains / Legend of Dragoon
restoration project) with FFmpeg integration for FMV playback. Same
underlying recompiler approach, same BIOS-stubbing problem.
Abandoned. Contains many FMV decode test artifacts.

### v3 — `F:/Projects/psxrecomp-projects-v3/`

The "this time we'll do BIOS first properly" restart. **It was not.**
v3's actual architecture, verified by reading the source:

1. **The recompiler (`psxrecomp/recompiler/`) only ingests PS1-EXE
   files.** `main_psx.cpp:17` calls `PS1ExeParser::parse_file()` which
   validates a `"PS-X EXE"` magic header. SCPH1001.BIN has no such
   header. Searching the entire `recompiler/` tree for `SCPH`,
   `bios.bin`, `0xBFC00000`, or any BIOS-related identifier returns
   zero matches. **The recompiler in v3 has never seen the BIOS ROM.**

2. **`TombaRecomp/generated/` contains only `SCUS_942.36_*.c`** (Tomba's
   game serial). No `SCPH1001*.c` file has ever been emitted.

3. **At runtime, the BIOS is executed by `runner/src/interpreter.c`**, a
   919-line hand-written MIPS R3000A interpreter. `main_runner.cpp:575`
   calls `interpreter_run_bios_boot(&g_cpu, bios_only)` which walks the
   bytes of SCPH1001.BIN starting at `0xBFC00000`. This is exactly what
   an emulator does.

4. **`runner/src/bios.c` is 1808 lines of HLE shims** that intercept
   the BIOS A0/B0/C0 vectors and replace real BIOS functions with C
   reimplementations. Many shims fake outputs. The most egregious
   example, found while writing this plan: `B0:0x4B (StartCard)` at
   `bios.c:1340` sets an EvCB status byte directly with the comment
   *"no SIO chain handler — deliver completion event manually"*. The
   real SIO chain handler was never installed because the integration
   between the interpreter and the IRQ delivery path is incomplete.
   Hand-delivering the event made the symptom go away while leaving
   the underlying bug untouched.

5. **The "run native" path inside `bios.c` does not run native C
   either.** `bios_run_native_b0` (line 284) reads the B0 dispatch
   table from PS1 RAM, finds the function address, and calls
   `interpreter_run` on it. "Native" in v3 means *"interpreted MIPS
   bytes from the real BIOS ROM"* — not *"compiled C function"*. This
   redefinition of "native" is itself a symptom of the architectural
   confusion.

6. **555 GB of `boot_trace*.log` and `card_test*.log` files** had
   accumulated in `TombaRecomp/` because someone left a tracing flag
   on for hours/days. Deleted before forking.

v3 ran for ~25 commits and reached "Sony logo displays + BIOS shell
appears + memory card screen renders" — but every milestone past the
Sony logo depended on increasingly elaborate HLE deception. The fix
for each new bug was another shim. The architecture made bugs cheaper
to paper over than to fix.

**The v3 failure was not a discipline failure. It was an architectural
failure.** The CLAUDE.md rules in v3 forbade stubs, forbade printf,
forbade game-loading, forbade simulating outputs of native systems —
and every rule was violated because the architecture made compliance
impossible. You cannot run an interpreter+HLE system without making
the integration choices that *look* like stubs.

---

## Mistakes we are not repeating in v4

The following are concrete things v3 (and v1, v2) did that v4 will
architecturally prevent:

### M1. The recompiler ignored the BIOS

v3's recompiler was hard-coded to PS1-EXE input. It could not have
ingested SCPH1001.BIN even if a session had tried. The infrastructure
(basic block analysis, control flow, function discovery, code
generation) all worked on raw MIPS bytes — but the entry point gated
everything behind `PS1ExeParser::parse_file`, which rejected the BIOS
ROM.

**v4 fix:** the recompiler ships with a new entry point `main_bios.cpp`
that ingests a flat 512 KB ROM at `0xBFC00000`. The PS1-EXE entry
point is preserved as `main_psx.cpp` for the eventual game phase, but
`main_bios.cpp` is the *primary* tool until Phase 5.

### M2. "Native" was redefined to mean "interpreted from ROM bytes"

v3's `bios_run_native_b0/c0` helpers read the BIOS dispatch table at
runtime, found the target function in RAM, and ran it through the
MIPS interpreter. Sessions started using the word "native" to mean
this — and over time, "make this function native" came to mean "stop
faking it in HLE C, instead let the interpreter run the real ROM
bytes". This is still emulation. It is not recompilation.

**v4 fix:** there is no interpreter in v4. The word "native" means
"a compiled C function emitted by the recompiler". If a function is
not yet recompiled, the program cannot run that function — it
fatally errors and you go fix the recompiler. There is no fallback.

### M3. HLE BIOS layer with selectively-faked outputs

v3's `bios.c` was 1808 lines of `case` statements over BIOS function
indices, each containing either (a) a C reimplementation of the BIOS
function from nocash specs, (b) a degenerate `cpu->v0 = 0; return;`
stub, (c) a delegation to the interpreter, or (d) a "fake the output"
hand-delivery of the kernel state the real BIOS would have produced.
Type (d) is the worst because it looks like progress.

**v4 fix:** there is no HLE BIOS layer. The recompiled SCPH1001.BIN C
output IS the BIOS. There is no second source of truth for `OpenEvent`
or `StartCard` or `alloc_kernel_memory`. The recompiler emits whatever
the BIOS ROM does, byte for byte semantically.

### M4. Kernel state was simulated in C alongside the interpreter's
copy

v3 had `events.c` (273 lines) and `threads.c` (149 lines) which
maintained C-side copies of the BIOS's EvCB and TCB tables. These
were supposed to mirror what the BIOS itself stored in RAM at the
addresses discovered during boot, but in practice the C copies and
the in-RAM copies drifted constantly because every HLE shim updated
one or the other but rarely both. v1's save/load failure had the
same root cause: the kernel data structures the game saw were not
the kernel data structures the BIOS thought existed.

**v4 fix:** the recompiled BIOS allocates and manages its own EvCB and
TCB tables in PS1 RAM, just like the real BIOS does. There is no
parallel C-side mirror. The hardware simulation reads/writes RAM at
those addresses; the recompiled BIOS code does too; they share state
through the actual RAM array.

### M5. Stub creep under schedule pressure

Every prior version started with "no stubs ever" in CLAUDE.md and
ended with hundreds of stubs. The reason: when a new BIOS path
crashes the interpreter, the easy fix is to add an HLE case that
returns a plausible value. The hard fix is to debug why the
interpreter crashed in real BIOS code. The cheap fix wins under
schedule pressure, every time, because it makes the immediate symptom
go away.

**v4 fix:** the recompiler emits or it doesn't. There is no
"intermediate" state where a function is partly recompiled. If the
recompiler can't emit a function, the build fails. There is no place
to put a stub.

### M6. 555 GB of debug exhaust

v3 violated its own no-printf rule and accumulated 555 GB of trace
log files because somebody left a tracing flag on. The rule was
correct. The rule was being ignored. The architecture made
fprintf-debugging *useful* (because the interpreter was the only
inspectable thing), so the rule lost.

**v4 fix:** v4 will not have an interpreter for the BIOS. The thing
to inspect at runtime is the recompiled C, which can be inspected with
a normal native debugger (gdb, MSVC) at full speed. There is no
"trace every interpreter step" because there is no interpreter. The
TCP debug server (Phase 2 onward) is the only structured runtime
inspection mechanism, and it streams over a socket — it does not write
files.

### M7. Project name was a lie

The project was called "PSXRecomp" but the BIOS execution path was a
runtime emulator. The name implied recompilation; the architecture
delivered emulation. This created a cognitive dissonance where every
session reasoned "we're a recomp project, so we should use the
recompiler" — but there was no path from the recompiler to the BIOS,
so the BIOS work always happened in the runner's HLE/interpreter
layer instead. The name pulled toward A; the code pulled toward
emulation.

**v4 fix:** name and architecture aligned. The recompiler is the
primary tool. The runner is a thin SDL+MMIO+debug-server shell that
hosts the recompiled output. If you find yourself writing more than
a few hundred lines of C in the runner, you are probably building the
wrong thing.

---

## Architecture A — what v4 actually is

### The pipeline

```
+--------------------+      +----------------+      +----------------------+
| bios/SCPH1001.BIN  | ---> | recompiler     | ---> | generated/           |
| (512 KB MIPS code) |      | (MIPS -> C)    |      |   SCPH1001_full.c    |
+--------------------+      +----------------+      |   SCPH1001_dispatch.c|
                                                    +----------+-----------+
                                                               |
                                                               v
                            +-----------------------+    +---------------+
                            | runtime/              |    | C compiler    |
                            |  - SDL2 main loop     +--->| (MSVC / gcc)  |
                            |  - memory map         |    +-------+-------+
                            |  - hardware sims      |            |
                            |  - debug server       |            v
                            +-----------------------+    +-------+-------+
                                                         | bios_runner   |
                                                         | (native exe)  |
                                                         +---------------+
```

### The recompiler's job

1. Load `SCPH1001.BIN` as a flat 512 KB byte array at base address
   `0xBFC00000`.
2. Discover function entry points. Initial seed: the reset vector at
   `0xBFC00000`, the exception vectors at `0xBFC00100`/`0xBFC00180`,
   and the A0/B0/C0 dispatch sites at `0xA0`/`0xB0`/`0xC0` in RAM
   (after the BIOS copies code into RAM during early init — see "self-
   modifying boot code" below).
3. For each discovered function, run the existing v3-salvaged
   `BasicBlockAnalyzer` and `ControlFlowAnalyzer` to find its blocks
   and edges.
4. Translate each block into C using the existing
   `CodeGenerator::translate_*` methods, **plus extensions** for COP0
   kernel-mode instructions and COP2 (GTE) instructions the BIOS uses.
5. Emit a `SCPH1001_full.c` file containing one C function per BIOS
   function, named by entry address (e.g., `func_0xBFC00000`).
6. Emit a `SCPH1001_dispatch.c` file containing a PC-to-function-pointer
   table so indirect calls (`jr $t0`, `jalr $t1`) can resolve at
   runtime.

### The runtime's job

1. Allocate and initialize PS1 memory (2 MB main RAM, 1 KB scratchpad,
   512 KB BIOS ROM region populated from `bios/SCPH1001.BIN`).
2. Provide MMIO read/write entry points that the recompiled BIOS C
   code calls into (e.g., `psx_mmio_read_word(0x1F801070)` for I_STAT).
3. Simulate the hardware peripherals the BIOS touches during boot:
   - Interrupt controller (I_STAT, I_MASK)
   - Timers (root counters 0-2)
   - GPU command processor + software rasterizer (for the Sony logo)
   - DMA channels (BIOS uses GPU DMA for the logo)
4. Drive an SDL2 window and pump events.
5. Expose a TCP debug server for inspection (no printf, ever).
6. **Call into the recompiled BIOS** by jumping to `func_0xBFC00000`
   (the reset vector entry function) in the generated C. The BIOS
   then runs to completion the same way it does on real hardware.

### What the runtime does NOT do

- It does not interpret MIPS bytes. There is no interpreter.
- It does not implement BIOS functions in C. There is no HLE.
- It does not maintain shadow kernel state. EvCB, TCB, kernel
  workspace — all of those live in PS1 RAM, written by the recompiled
  BIOS code, read by the recompiled BIOS code.

---

## What we salvaged from v3

The full file inventory of v3 is documented separately (we have it).
This section explains specifically what survived the fork.

### Salvaged: `recompiler/` (the entire subtree)

Copied verbatim from `psxrecomp-projects-v3/psxrecomp/recompiler/`,
build cache excluded. This is roughly 3,800 lines of C++ implementing:

| File | Purpose | Reused as-is? |
|---|---|---|
| `src/mips_decoder.cpp` | MIPS field extraction, register names, branch math | Yes |
| `src/basic_block.cpp` | Leader-finding, basic block construction | Yes |
| `src/control_flow.cpp` | Branch/jump identification, successor edges | Yes |
| `src/function_analysis.cpp` | Prologue/epilogue heuristics for function discovery | Yes (with one extension — see below) |
| `src/code_generator.cpp` | Per-instruction MIPS-to-C string emission | **Reused with extensions** for COP0 |
| `src/annotations.cpp` | TOML-based recompilation hints | Yes |
| `src/main_psx.cpp` | PS1-EXE entry point | Kept for Phase 5; **not used in Phase 1** |
| `src/ps1_exe_parser.cpp` | PS-X EXE header parser | Kept for Phase 5; not used in Phase 1 |
| `lib/rabbitizer/` | MIPS disassembler dependency | Yes |
| `lib/fmt/` | C++ string formatting | Yes |
| `lib/toml11/` | TOML parser for annotations | Yes |

### Salvaged: `bios/SCPH1001.BIN`

Copied from `psxrecomp-projects-v3/TombaRecomp/isos/SCPH1001.BIN`.
524288 bytes (512 KB), the standard SCPH1001 BIOS image. This is the
recompilation target.

### Salvaged: `duckstation/`

**Moved** (not copied) from `F:/Projects/psxrecomp-projects/duckstation/`.
~2.1 GB. This is a modified DuckStation build with TCP debug server
extensions added during v3 (debug protocol on port 4371). It is the
runtime oracle for verifying that the recompiled BIOS produces the
same hardware register sequences, the same VRAM contents, and the
same kernel state as a real PS1 BIOS execution does.

### NOT salvaged from v3

The `psxrecomp-projects-v3/psxrecomp/runner/` subtree, **23 source files
and 12,852 lines of C/C++**, was entirely excluded from the fork.
Specifically:

| v3 file | Why excluded |
|---|---|
| `runner/src/interpreter.c` | The MIPS interpreter. v4 has no interpreter. Discarded. |
| `runner/src/bios.c` | 1808 lines of HLE shims. The disease. Discarded. |
| `runner/src/events.c` | Shadow EvCB management. The recompiled BIOS will manage its own. |
| `runner/src/threads.c` | Shadow TCB management. Same reason. |
| `runner/src/bios_trace.c` | Per-call HLE trace logger. Not meaningful in v4. |
| `runner/src/func_logger.c` | Runtime function discovery from interpreter execution. Useless without interpreter. |
| `runner/src/main_runner.cpp` | SDL window + interpreter driver. Will be rewritten as a much smaller `main.cpp` in Phase 2. |
| `runner/src/ps1_exe_parser.cpp` | PS1-EXE loader. Game-EXE thing, defer to Phase 5. |

### Eligible for salvage in Phase 2 (one file at a time, audited)

These are hardware simulation files — they implement PS1 peripherals,
not BIOS functions. They will be needed by the runtime to service the
MMIO calls the recompiled BIOS makes. They will be **copied one at a
time, audited, and only the parts that are pure hardware sim are
kept.** Anything that touches `events_get_base()`, `bios_*`,
`interpreter_*`, kernel state mirrors, or "if HLE then else" branching
gets stripped out before the file enters v4.

| v3 file | What it does | Phase needed |
|---|---|---|
| `runner/src/memory.c` | RAM/ROM/scratchpad/MMIO routing | Phase 2 |
| `runner/src/interrupts.c` | I_STAT/I_MASK + COP0 IRQ delivery | Phase 2 |
| `runner/src/timers.c` | Root counters 0-2 | Phase 2 |
| `runner/src/dma.c` | 7 DMA channels | Phase 2 |
| `runner/src/gpu.c` | GP0/GP1 command processor | Phase 3 (Sony logo) |
| `runner/src/gpu_sw_renderer.c` | Software rasterizer | Phase 3 |
| `runner/src/debug_server.c` | TCP debug server | Phase 2 |
| `runner/src/crc32.c` | Helper | Phase 2 |
| `runner/src/sio.c` | SIO0 controller (pad + memcard protocol) | Phase 4 (BIOS shell input) |
| `runner/src/memcard.c` | Memory card image format | Phase 6 (saves) |
| `runner/src/cdrom.c` | CD-ROM controller | Phase 5 (game loading) |
| `runner/src/iso_reader.cpp` | ISO9660 reader | Phase 5 |
| `runner/src/gte.cpp` | GTE math (well-tested) | Phase 5 (3D rendering) |
| `runner/src/spu.c` | SPU register shadow (no audio) | Phase 5+ |

---

## Hard engineering problems v4 must solve

These are the things we know are non-trivial. We want outside review on
how to approach them.

### HP1. COP0 kernel-mode instructions in `code_generator.cpp`

The PS1 BIOS uses COP0 instructions that v3's `code_generator.cpp`
does not translate (verified by reading the file). Specifically:

- `mtc0 rt, rd` / `mfc0 rt, rd` — move to/from COP0 registers
- `rfe` — return from exception (restores SR's IE/KU stack)
- COP0 register reads/writes for SR (12), Cause (13), EPC (14),
  BadVaddr (8)

These are used by the BIOS exception handler at `0x80000080` and by
the boot init code (cache isolation, IRQ enable/disable). v3 didn't
need them in the recompiler because v3 only recompiled game EXEs,
which run in user mode and never touch COP0.

**Question for review:** what's the cleanest way to model COP0 in the
generated C? Options we see:
1. A `cop0` struct in `CPUState` with explicit fields (`sr`, `cause`,
   `epc`, etc.) and translation methods that emit
   `cpu->cop0.sr = ...;`
2. A 32-entry `cop0_regs[32]` array with helpers, mirroring how the
   GPRs work
3. A function-call-per-instruction approach (`cpu_mtc0(cpu, rt, rd)`)
   so the runtime can intercept SR writes for IRQ delivery

Option 3 feels cleanest because IRQ delivery needs a hook on SR
writes anyway, but it adds a function call per COP0 access.

### HP2. The BIOS does self-modifying boot code

During the first ~10000 instructions after the reset vector, the PS1
BIOS does the following:

1. Copies a chunk of code from BIOS ROM (`0xBFC0xxxx`) into RAM
   (`0x80000000` + offset) — typically the kernel/exception/vector
   support code.
2. Patches A0/B0/C0 dispatch addresses at fixed RAM addresses `0xA0`,
   `0xB0`, `0xC0` to point at the in-RAM code.
3. Builds the A0/B0/C0 jump tables in RAM at addresses determined at
   runtime by where the kernel was loaded.

This is a problem for static recompilation because:

- The "in-RAM" version of the kernel functions is the same MIPS
  bytes as the "in-ROM" version, but at a different address. A naive
  recompiler would emit two copies under different function names.
- The A0/B0/C0 jump tables are built at runtime, so the targets of
  `jal A0`, `jal B0`, `jal C0` (the standard BIOS function call
  pattern) are not statically resolvable from the ROM bytes alone.
  You have to either (a) recognize the table-build pattern and
  statically resolve the targets, or (b) emit indirect-call code that
  reads the table at runtime.

**Question for review:** how do other BIOS-like static recomp projects
handle (b)? In particular, is there a clean way to express
"statically known indirect dispatch through a table built at runtime"
in generated C? We are inclined to:

- Emit the A0/B0/C0 dispatch sites (`0xA0`, `0xB0`, `0xC0`) as
  indirect-call functions that look up a global function pointer
  table, e.g. `dispatch_a0[func_index]`.
- Recognize the table-build code in the BIOS init and statically
  resolve which table entries point at which recompiled functions.
- Populate the dispatch tables from generated C startup code, before
  the recompiled BIOS reset vector runs.

This avoids running the table-build code at all. The tradeoff is that
the recompiler needs to *understand* the table-build pattern, which
makes it BIOS-specific (vs. fully generic MIPS-to-C). Is there a more
generic option?

### HP3. The RAM-vs-ROM duplicate code problem

Related to HP2: the BIOS copies code from ROM to RAM and then runs the
RAM version. The instructions are byte-identical but addresses
differ. What does the recompiler do?

Options:
1. **Emit only the ROM version**, and statically resolve the RAM-side
   addresses to the same functions. (Requires the recompiler to know
   the ROM->RAM copy mapping.)
2. **Emit both versions** as separate functions with different names,
   and rely on the BIOS init code to "select" the right version at
   runtime. (Wastes code; runtime selection still needs an indirect
   call.)
3. **Emit only the RAM version** and recompile from a snapshot of
   RAM after the BIOS init copy. (Requires running the BIOS once to
   capture the RAM state, then recompiling against that — circular.)

We are inclined to option 1, with the recompiler understanding the
specific ROM->RAM copy that the BIOS does early in init (it's a
well-known pattern and the source/dest/length are constants in the
BIOS code).

### HP4. Exception vector at `0x80000080`

The PS1 general exception vector is at `0x80000080` in RAM. After the
BIOS init copies the exception handler into place, every interrupt
and every syscall jumps to `0x80000080`. The handler at that address
saves COP0 state, looks up the cause, dispatches to a chain handler
list (separate per IRQ source), and then `rfe`s back.

For v4 this means:

- The recompiler must emit a function for `0x80000080` whose entry is
  callable from runtime IRQ delivery.
- The runtime must call that function whenever an IRQ fires.
- The recompiled exception handler will then dispatch to the chain
  handlers (which are themselves recompiled BIOS functions registered
  via the BIOS chain API at boot).
- Save/restore of CPU state across the exception is handled by the
  generated C (via `mtc0`/`mfc0`/`rfe` translations).

**Question for review:** is there an idiomatic way to model
"interrupt arrives during normal C execution, transfer to exception
handler function, return when handler is done" in C? We are thinking
the runtime sets a `cpu->irq_pending` flag, the generated C checks
that flag at function entry / loop back-edges (similar to how N64Recomp
handles RSP signals), and on transition it directly calls the exception
function. This avoids needing real signals or threads.

### HP5. Cache isolation for the kernel data area

The PS1 doesn't have a real MMU. It has a quirk: when COP0 SR bit 16
("Isc" — isolate cache) is set, all loads/stores are routed to the
D-cache instead of memory. The BIOS uses this to repurpose the D-cache
as a 1KB scratchpad for kernel data (PCB pointer, TCB pointer, etc.).
The "scratchpad" lives at `0x9F800000` in cached KSEG0 (or
`0xBF800000` uncached).

For v4 this means:

- `mtc0` to SR with bit 16 set must change a runtime "cache isolated"
  flag that affects subsequent load/store routing.
- Loads/stores while isolated go to a 1 KB scratchpad buffer, not to
  main RAM.
- The kernel data layout in that scratchpad is what `events.c` and
  `threads.c` were trying to mirror in v3 — and getting wrong.

In v4, the recompiled BIOS will write to the real scratchpad through
the normal load/store path, the runtime simulates cache isolation
correctly, and there is no shadow C-side state to drift.

### HP6. GTE (COP2) instructions

The BIOS itself does not use the GTE — that's a game-side concern. So
GTE translation is **not** required for Phases 1-4. It is required
for Phase 5+ (game EXE recompilation), at which point the
recompiler's `code_generator.cpp` gains COP2 instruction handling.
v3 had a working `gte.cpp` (876 lines, well-tested) that can be
reused at that time.

---

## Phase plan

## Phase 1 — Controlled Recompiler Bring-up

Phase 1 is the *only* phase in v4 that owns the recompiler-side proof
of correctness. It exists to make the recompiler trustworthy on the
BIOS ROM **before** anything else (runtime, MMIO, hardware sims) is
allowed to be built. Phase 1 is split into strict sub-phases. Each
sub-phase has REQUIRED ARTIFACTS, FAILURE CONDITIONS, and ACCEPTANCE
CRITERIA. A sub-phase is not "done" until every acceptance criterion
holds and every required artifact exists on disk.

### Phase 1 hard constraints (apply to ALL sub-phases below)

- **No full BIOS walk before Phase 1e is complete and merged.** Walking
  the entire ROM before relocation is solved will silently emit
  duplicate functions for ROM and RAM aliases, and that is exactly
  the v3 failure mode resurrected at the codegen layer. Bounded walks
  only.
- **`strict_translator` is the only valid translation path.** It
  fails loudly on unknown opcodes, unknown control flow, or any
  ambiguity. The salvaged v3 `code_generator.cpp` is **not trusted**
  and must not be invoked from any new code path. If a sub-phase
  needs an instruction that the strict translator does not yet
  handle, the fix is to extend the strict translator with a real
  implementation — never to fall back to the salvaged path, never
  to add a default case, never to "absorb" the salvaged path's
  behavior into the strict path opaquely. The strict translator
  absorbs salvaged code only by transcribing each opcode explicitly.
- **No silent opcode skipping. No fallthrough cases. No "default:
  emit comment".** An unsupported opcode is a build failure with the
  address and the opcode recorded in `generated/unsupported_ops.json`.
- **No partial instruction implementations.** A handler that
  implements 80% of an instruction's semantics is not 80% done; it
  is 0% done and a stub.
- **No hand-editing of files under `generated/`.** Generated code is
  a build artifact. Fix the recompiler.
- **Every sub-phase must produce its manifest before it is considered
  complete.** Code without a manifest is not progress; it is a claim
  without proof.

---

### Phase 1a — Boot Slice (COMPLETE — 2026-04-06)

**Status:** COMPLETE. Do not redo. Do not "improve". Do not extend its
scope. Phase 1a is frozen as a reference baseline; later sub-phases
depend on its outputs being stable.

**What was delivered:**

- `recompiler/src/strict_translator.{h,cpp}` — the only sanctioned
  MIPS-to-C path in v4.
- `recompiler/src/main_bios.cpp` — BIOS flat-ROM ingestion entry
  point. Loads `bios/SCPH1001.BIN` as 524288 bytes at virtual base
  `0xBFC00000`.
- `generated/boot_slice.c` — 30 instructions, region
  `0xBFC00000..0xBFC00074`, terminating at `j 0xBFC00150`.
- `generated/boot_slice_manifest.json` — function entry, instruction
  count, end address, termination reason.
- `generated/unsupported_ops.json` — empty (no unsupported instructions
  in the slice).
- `generated/cpu_state.h` — compile-only `CPUState` definition. **It
  must remain compile-only.** No runtime, no MMIO, no peripherals
  may be added to this file. It exists so generated C compiles in
  isolation; that is the entire job.

**Decisions locked by 1a (do not relitigate):**

- `RFE` substitutes for the nonexistent "ERET" in the strict
  translator's terminator list. R3000A has no ERET. Approved
  2026-04-06.
- The strict translator owns its own opcode dispatch and will never
  delegate to the salvaged `code_generator.cpp`.

**Acceptance (already met):** `boot_slice.c` compiles standalone with
`-c`, manifest is complete, `unsupported_ops.json` is empty, every
instruction in the slice is accounted for in the manifest, no TODO
or FIXME exists in the generated output.

---

### Phase 1b — Instruction Coverage (NEXT)

**Goal:** every MIPS R3000A instruction the BIOS uses inside any region
the recompiler will ever walk has a real, complete implementation in
`strict_translator`. No instruction the BIOS executes is allowed to
exist in the "unknown" set when Phase 1b ends.

**Method (bounded — no full walk yet):**

1. Use Ghidra (`SCPH1001.BIN` at `0xBFC00000`) as the static oracle
   for the instruction inventory. Enumerate every distinct opcode
   that appears in the disassembly of the ROM. This is a static
   read-only query against the Ghidra MCP — it does not require
   walking the BIOS in v4's recompiler.
2. For each opcode in the inventory, check whether
   `strict_translator` already implements it. The check is by
   reading the dispatch table in `strict_translator.cpp` — not by
   trial-emission and "looks fine".
3. For each missing opcode, add a real implementation. COP0
   instructions (`mtc0`, `mfc0`, `rfe`, and the SR/Cause/EPC/BadVaddr
   register accesses) are required because the BIOS exception
   handler uses them. COP2/GTE is **not** required in Phase 1 — it
   is a Phase 5 concern (game side).
4. After each opcode addition, regenerate `boot_slice.c` and confirm
   it still produces the byte-identical manifest from Phase 1a. Phase
   1a is the regression baseline.

**Required artifacts:**

- `generated/instruction_inventory.json` — every distinct opcode
  observed in `SCPH1001.BIN` according to Ghidra, with the count of
  occurrences and at least one example address per opcode.
- `generated/instruction_coverage.json` — for each opcode in the
  inventory: `implemented` (true/false), the source location of the
  handler in `strict_translator.cpp` if implemented, and the
  rationale ("required for BIOS execution path", "GTE — deferred to
  Phase 5", etc.) if intentionally deferred.
- An updated `strict_translator.cpp` whose dispatch covers every
  opcode marked `implemented: true` in the coverage file.
- `generated/boot_slice.c` and `generated/boot_slice_manifest.json`
  must regenerate identically to the Phase 1a baseline (regression
  proof).

**Failure conditions (any one of these means Phase 1b is NOT done):**

- The instruction inventory is missing any opcode that Ghidra
  reports in the ROM.
- Any opcode marked `implemented: true` in the coverage file lacks a
  real handler in `strict_translator.cpp` (a `default:` branch, a
  fallthrough, a comment-only emit, or a "TODO" all count as no
  handler).
- Any opcode the BIOS uses is silently deferred to Phase 5 without
  an explicit, justified entry in the coverage file.
- The Phase 1a regression baseline drifts.
- The salvaged `code_generator.cpp` is invoked from any new code
  path.
- `unsupported_ops.json` is not empty after a clean rebuild of the
  Phase 1a slice.

**Acceptance criteria (ALL must hold):**

- `instruction_inventory.json` exists and matches the Ghidra opcode
  inventory.
- `instruction_coverage.json` exists and lists every inventory opcode
  with an implemented or explicitly-deferred status.
- Every BIOS-required opcode is implemented in `strict_translator`.
- Phase 1a `boot_slice.c` regenerates identically.
- `unsupported_ops.json` is empty.

---

### Phase 1c — Function Discovery Pipeline

**Goal:** a deterministic, artifact-producing function discovery
pipeline that, given a *bounded* set of seed addresses, walks each
function exactly once, records every basic block, and emits a manifest
of every function it discovered along with how it was discovered and
how it terminated. This is still a bounded walk — Phase 1c does NOT
walk the entire ROM. It walks the seeds the user gives it.

**Method:**

1. Define the seed set as an explicit input file
   (`recompiler/seeds/phase1c_seeds.json`). The initial seeds for
   bring-up are: the reset vector (`0xBFC00000`) and the BEV=1
   general exception vector (`0xBFC00180`). No other seeds are
   added until Phase 1d/1e prove the relocation story.

   **Empirical correction (2026-04-06):** an earlier draft of this
   plan listed `0xBFC00100` as a third seed under the label "ROM-side
   exception vector". That assertion was wrong and is removed.
   `0xBFC00100` is the R3000 UTLB-miss vector when BEV=1, but PS1's
   R3000A has no TLB, so the address is never executed in normal
   operation. Phase 1b's instruction-inventory walker was run with
   `0xBFC00100` as a synthetic seed to test the assertion; the walker
   immediately bled into the bytes at `0xBFC00108`, which Ghidra's
   `list_data` already classifies as the ASCII string `"Sony Computer
   Entertainment Inc."`. The walker decoded those string bytes as
   garbage opcodes (top-level opcodes `0x18`, `0x19`, `0x1B`, `0x1D`,
   `0x1E` — all unallocated on R3000A). The proof is in
   `generated/instruction_inventory.json` history and in the seed-list
   selection inside `tools/build_instruction_inventory.py`. The
   canonical Phase 1c seed list is therefore exactly two ROM addresses:
   `0xBFC00000` and `0xBFC00180`. `0xBFC00100` must NOT be re-added
   without new evidence that some BIOS path actually executes it.
2. For each seed, run the strict basic-block / control-flow analysis
   on the ROM bytes. Record every basic block, every edge, every
   terminator, every callee discovered via a direct `jal` (direct
   `jal` only — indirect dispatch is Phase 1d).
3. Functions discovered via direct `jal` from a seed are themselves
   added to the discovery worklist. Each function is walked once
   and only once, identified by its normalized address (see Phase
   1e for the normalization rule).
4. The walk terminates a function on the first of: unconditional
   branch out of function, `jr $ra`, `rfe`, or any indirect jump
   (the indirect jump is recorded for Phase 1d but does not get
   resolved here).

**Required artifacts:**

- `recompiler/seeds/phase1c_seeds.json` — the explicit seed list
  used for the run.
- `generated/function_manifest.json` — for every discovered function:
  entry address, normalized address, instruction count, end address,
  termination reason (`jr_ra`, `rfe`, `j_out_of_function`,
  `indirect_jump_recorded_for_1d`, ...), the seed lineage that
  reached it (e.g. "reset_vector → direct_jal at 0xBFC00050"), and a
  list of basic-block leader addresses.
- `generated/function_edges.json` — every direct call edge
  (`caller_addr`, `caller_pc`, `callee_addr`).
- `generated/discovery_run.log.json` — a structured (NOT free-text)
  log of the discovery run: counters, opcode histogram, time spent
  per function. **Structured JSON only — never `fprintf`.**

**Failure conditions:**

- The walker visits any function more than once under the same
  normalized address.
- A function is recorded without a termination reason.
- Any seed is implicit. Every seed must come from
  `phase1c_seeds.json`.
- The walker silently drops a function that hits an unsupported
  opcode. (It must hard-fail and the opcode must appear in
  `unsupported_ops.json` — that is what Phase 1b exists to prevent.)
- The walker continues past an indirect jump under the assumption
  that the next linear instruction is reachable. Indirect jumps
  terminate the walk for this function in Phase 1c.

**Acceptance criteria:**

- Running the recompiler against the Phase 1c seed file produces
  every required artifact above.
- `function_manifest.json` contains the reset vector and both ROM
  exception vectors with full metadata.
- Every function in the manifest has an explicit termination
  reason from the allowed set.
- No function appears twice under the same normalized address.
- `unsupported_ops.json` remains empty.

---

### Phase 1d — Indirect Control Flow Recording

**Goal:** every indirect jump (`jr` other than `jr $ra`, every `jalr`)
encountered during the Phase 1c walk is recorded with full context, so
that Phase 1e and later phases have the data needed to resolve them.
Phase 1d does NOT resolve indirect jumps. It only records them. This
is deliberate — resolving them before relocation is solved leads to
the v3 failure mode.

**Method:**

1. Extend the Phase 1c walker to capture, at every indirect jump
   site: the source PC, the source function, the source basic block,
   the register the indirect jump uses, the last ~16 instructions
   leading up to the jump (so a future resolver can run lightweight
   value tracking on the GPR), and the calling-convention guess
   (`computed_call`, `computed_tail_call`, `dispatch_stub`,
   `unknown`).
2. Cross-reference every recorded site against Ghidra's analysis of
   the same address (`mcp__ghidra__get_function_info`,
   `mcp__ghidra__xrefs`) and store Ghidra's view alongside the
   recompiler's view. Disagreement between Ghidra and the recompiler
   is itself a finding that must be surfaced — it is never silently
   reconciled.
3. Classify each site by the *kind* of resolution it will eventually
   need: A0/B0/C0 vector dispatch through a runtime-built table,
   `jr $ra` return, computed jump within a jump table, or unknown.
   "Unknown" is a valid and expected value at this stage.

**Required artifacts:**

- `generated/indirect_jumps.json` — one entry per site with every
  field above.
- `generated/indirect_jump_classes.json` — counts per classification
  bucket and the list of sites in each bucket.
- `generated/indirect_jump_ghidra_xref.json` — Ghidra's view of each
  site (function containing it, Ghidra's resolved targets if any,
  Ghidra's note on the dispatch shape).

**Failure conditions:**

- A `jr` or `jalr` is encountered during the walk and is not
  recorded.
- A site is recorded without its source function context or its
  preceding-instruction window.
- A site is "resolved" by Phase 1d. Resolution belongs to a later
  phase. Phase 1d guesses nothing.
- An indirect jump is silently classified as "return" without a
  proof that the register is `$ra` and the function epilogue
  matches the standard pattern.

**Acceptance criteria:**

- Every indirect jump in every Phase 1c-discovered function is in
  `indirect_jumps.json`.
- Every entry has a classification (including `unknown`).
- Ghidra cross-reference exists for every entry.
- `unsupported_ops.json` is still empty.

---

### Phase 1e — Relocation Planning (HARD GATE)

**Goal:** the ROM→RAM aliasing problem is solved on paper and proven
with artifacts before any code is allowed to walk the full BIOS. After
Phase 1e ends, the recompiler treats ROM and RAM addresses that map
to the same logical instructions as the same logical function — and
that fact is provable, not assumed.

**Method:**

1. Identify, in `SCPH1001.BIN`, every memcpy/move loop that the BIOS
   executes during early init to copy code from `0xBFCxxxxx` into
   `0x800xxxxx`. Use Ghidra for the static side and DuckStation's
   memory trace for the runtime side. **Both sources must agree.**
2. For each identified copy: record source ROM range, destination
   RAM range, length, the function in the BIOS that performs the
   copy, and the proof artifacts (Ghidra screenshot/JSON of the
   copy loop, DuckStation memory trace excerpt).
3. Define the address normalization rule (the working draft is
   `normalized = address & 0x1FFFFFFF`, matching the KSEG/KUSEG
   stripping convention, but it must be checked against the actual
   ROM/RAM pairs found above and either confirmed or replaced).
4. Emit `generated/address_aliases.json` containing every proven
   `BFCxxxxx ↔ 800xxxxx` pair, and only those. No alias may be
   added without an entry in the proof artifact set.
5. Teach `strict_translator` and the discovery walker to use the
   normalized address as the function identity. Re-run Phase 1c
   discovery and confirm no function is emitted twice.

**Required artifacts:**

- `generated/relocation_proofs/` — a directory with one
  subdirectory per proven copy operation, each containing:
  - the Ghidra MCP output that identified the copy loop
  - the DuckStation memory-trace excerpt that confirmed it at
    runtime
  - a `proof.json` summarizing the source range, destination range,
    length, and the BIOS function that performs the copy
- `generated/address_aliases.json` — final alias table, generated
  *only* from proofs in `relocation_proofs/`
- `generated/normalization_rule.md` — the chosen normalization
  formula and the evidence that it matches every proven alias
- A regenerated `generated/function_manifest.json` (from a re-run
  of Phase 1c with normalization enabled) showing zero duplicate
  function bodies under normalized addresses

**Failure conditions:**

- An alias appears in `address_aliases.json` without a corresponding
  proof directory under `relocation_proofs/`.
- Ghidra and DuckStation disagree on a copy operation and the
  disagreement is reconciled by guessing.
- The normalization rule is asserted without the evidence file.
- Any function body is emitted twice under the same normalized
  address.
- "Looks equivalent" appears in any artifact.

**Acceptance criteria (ALL must hold):**

- Every copy the BIOS performs from ROM into RAM during init is
  documented in `relocation_proofs/` with both Ghidra and DuckStation
  evidence.
- `address_aliases.json` is generated mechanically from those proofs
  and contains nothing else.
- `normalization_rule.md` exists and is consistent with every alias.
- A re-run of Phase 1c under the new normalization rule produces no
  duplicate functions.
- `unsupported_ops.json` is still empty.

---

### Phase 1 exit gate (do not skip)

Full BIOS recompilation is **NOT** allowed before *all* of the
following are true on disk:

- Phase 1a baseline regenerates identically.
- `instruction_coverage.json` shows every BIOS-used opcode
  implemented.
- `function_manifest.json` exists from a normalized Phase 1c re-run.
- `indirect_jumps.json` exists and covers every indirect site.
- `address_aliases.json` exists and is backed by `relocation_proofs/`.
- `unsupported_ops.json` is empty.

If any of these is missing or stale, the next step is to fix it —
**not** to walk the rest of the ROM "to see what happens".

---

## BIOS Relocation Is a Hard Gate

This is a blocking requirement, not a suggestion. It is restated here
in its own section because every prior version of this project failed
to take it seriously.

- **ROM↔RAM aliasing must be solved before full BIOS recompilation.**
  The PS1 BIOS copies code from `0xBFCxxxxx` to `0x800xxxxx` during
  init and runs the RAM copy. A recompiler that does not understand
  this will silently emit two C functions for the same logical BIOS
  routine, under different names, with different identities, and
  every dispatch table built later will be wrong in a way that is
  almost impossible to debug.
- **Duplicate functions are forbidden.** A function is identified by
  its *normalized* address. If the recompiler ever emits two function
  bodies for the same normalized address, the build fails.
- **Alias mapping must be proven, not assumed.** Every entry in
  `generated/address_aliases.json` must trace back to a proof
  artifact under `generated/relocation_proofs/` containing both a
  Ghidra static identification of the copy loop and a DuckStation
  runtime memory trace confirming the copy at execution time. No
  proof, no alias.
- **The recompiler may not walk the full BIOS until Phase 1e is
  complete.** Bounded walks are allowed before 1e (single seeds,
  small slices). A full walk before 1e will re-import the v3
  architectural disease at the codegen layer.
- **"Seems equivalent" is not proof.** The normalization rule must be
  written down in `generated/normalization_rule.md`, must be
  consistent with every proven alias, and must be derived from
  evidence — never asserted because it "looks like" the standard
  KSEG masking.

If you find yourself wanting to walk the full BIOS "just to see what
breaks" before Phase 1e is done, **stop**. That is exactly the move
that broke v3.

---

## Proof-Driven Development

Phase 1 (and every later phase) operates under one rule: **no claim
without an artifact.**

- **All discoveries produce artifacts.** Every function found, every
  edge recorded, every indirect jump observed, every relocation
  identified produces a JSON file under `generated/`. The artifact
  is the discovery. The artifact is what gets reviewed. A claim made
  in chat that does not have a corresponding file on disk did not
  happen.
- **No claim is valid without output.** "I implemented `mtc0`" is
  not a claim that can be verified. "`strict_translator.cpp:412`
  implements `mtc0` and `instruction_coverage.json` shows it
  `implemented: true` and the Phase 1a slice still regenerates
  identically" is.
- **No "probably correct".** The forbidden language is the same as
  in the existing rules: "probably", "should work", "looks correct",
  "seems like", "matches expectation". Replace with proof or replace
  with the explicit string "unknown".
- **Unknown is acceptable. Guessing is not.** When a fact is not
  yet proven, the artifact records it as `unknown` with the reason
  it is unknown and what would be needed to resolve it. The artifact
  never lies about confidence.
- **Structured artifacts only.** JSON, not free-text. No log files.
  No `fprintf`. The artifact must be machine-readable so future
  sessions can diff it against current state and detect drift.
- **Artifacts are versioned with the code.** When a sub-phase is
  complete, its artifacts are part of the deliverable. Re-running
  the recompiler must regenerate them deterministically; if they
  drift from the committed copy without a code change, that itself
  is a finding.

The reason this section exists: every prior version of the project
had sessions in which Claude reported a result, the user believed it,
and the result turned out to be wishful synthesis with no underlying
artifact. v4 forbids that pattern by requiring the artifact to exist
*before* the result can be claimed.
### Phase 2 — Minimal runtime that hosts the recompiled BIOS

**Goal:** a runtime executable that links the Phase 1 output, calls
into `func_0xBFC00000` (the recompiled reset vector), and runs the
BIOS until it hits its first MMIO access. The MMIO access fatally
errors with a clear message ("BIOS read I_STAT @ 0x1F801070, no
hardware sim available").

**Concrete work:**
- Create `runtime/src/main.cpp` — minimal SDL2 window, no input, no
  rendering yet. Just a CPU state allocation and a single function
  call into the generated C.
- Create `runtime/src/memory.c` — RAM (2 MB), scratchpad (1 KB),
  BIOS ROM region (512 KB, populated from disk). Read/write entry
  points used by the generated C. **No MMIO routing yet** — any
  MMIO read/write fatally errors with the address.
- Create `runtime/src/cpu_state.h` — the `CPUState` struct that the
  generated C expects (32 GPRs, HI, LO, PC, COP0 registers).
- Build with the same compiler the recompiler emits for. Link
  against `SCPH1001_full.o` from Phase 1.
- Run. Capture which MMIO address the BIOS hits first.

**Success criterion:** the runtime starts, executes generated C for
some number of instructions, and dies with a clear "no hardware sim
for MMIO @ 0x..." message at a real BIOS MMIO read.

### Phase 3 — Hardware sims to reach the Sony logo

**Goal:** the BIOS reaches the GPU command sequence that draws the
Sony logo, and the logo appears on screen.

**Concrete work:**
- Salvage `interrupts.c`, `timers.c`, `dma.c`, `gpu.c`, and
  `gpu_sw_renderer.c` from v3, **one at a time**, audited for HLE
  state leakage. Wire each into `runtime/src/memory.c`'s MMIO
  routing.
- Wire `gpu_sw_renderer`'s VRAM into the SDL2 window's display buffer.
- Run. Capture every MMIO access and compare against DuckStation's
  TCP debug server output for the same boot sequence.
- Iterate until the Sony logo pixels appear in VRAM and on screen.

**Success criterion:** SDL2 window shows the Sony Computer
Entertainment logo. Pixels match DuckStation's VRAM byte-for-byte
(not just visually).

### Phase 4 — Boot to BIOS shell + memory card screen

**Goal:** BIOS shell renders, controller input works, memory card
screen renders correctly with no fake events anywhere.

**Concrete work:**
- Salvage `sio.c` from v3, audited.
- Salvage `memcard.c` from v3, audited.
- Wire SDL2 keyboard input through the runtime to the SIO controller
  state machine.
- Verify the BIOS scans for memory cards via the real SIO chain
  handler running as recompiled C. **Not via hand-delivered events.**
- Render the memory card management screen.

**Success criterion:** user can press buttons to navigate the BIOS
shell. Memory card screen displays. Cards present/absent correctly.
Format card works (writes 128 KB image, structure verified by
DuckStation comparison).

### Phase 5 — Game EXE recompilation layer

**Goal:** the recompiler can ingest a PS1-EXE (re-using the v3-era
`main_psx.cpp`) and emit `<game>_full.c`. The runtime can load that
recompiled game on top of the recompiled BIOS, with both linked into
the same binary.

**Concrete work:**
- Re-enable `recompiler/src/main_psx.cpp` (kept from v3 salvage).
- Add COP2 / GTE translation to `code_generator.cpp`. Reuse v3's
  `gte.cpp` for the math.
- Salvage `cdrom.c`, `iso_reader.cpp`, `iso_reader_c.cpp` for game
  disc loading.
- Salvage `spu.c` (silent SPU is fine for now).
- Recompile Tomba's game EXE. Link with the recompiled BIOS. Run.

**Success criterion:** Tomba boots to its title screen, runs
through the intro, reaches gameplay.

### Phase 6 — Save/load end-to-end (the v1 failure case)

**Goal:** the v1 / v2 / v3 failure mode of "save/load broken because
kernel state isn't real" does not happen. Saves go to a real 128 KB
memory card image, can be loaded by DuckStation, can be loaded by
real PS1 hardware, and can be loaded back by v4 itself.

**Success criterion:** save in v4, copy `.mcd` to DuckStation, load
and verify it's the same save. This is the milestone that all three
prior versions failed.

---

## Verification strategy

### DuckStation as the oracle

`duckstation/` (the modified version with TCP debug server, moved from
v3) serves as the runtime ground truth. For every milestone we run
the same BIOS boot sequence in v4 and in DuckStation, then diff:

- CPU register state at known PC checkpoints
- RAM contents at known kernel addresses (PCB, TCB, EvCB tables)
- VRAM contents (byte-for-byte)
- MMIO read/write sequences
- Timer counter values
- Interrupt fire ordering

The diff happens via TCP, not via log files. Both servers have a
ring buffer of state snapshots; the comparison tool walks both rings
and reports the first divergence.

### Ghidra as the static oracle

`SCPH1001.BIN` loaded into Ghidra at `0xBFC00000` provides the
source-of-truth disassembly. When the recompiler emits something
that looks wrong, the first check is "what does Ghidra say this
instruction does?" not "let me guess from context".

### The TCP debug server

In Phase 2 onward, the v4 runtime exposes a TCP debug server (port
4370 to match v3's convention, separate from DuckStation's 4371). It
provides commands for register dump, RAM read, VRAM dump, MMIO trace,
function call trace. **There is no fprintf logging anywhere in v4.**
If you need to inspect something, build a TCP command for it.

---

## Open questions for outside review

This document exists to be read by an external reviewer (ChatGPT or a
human) and to surface the architectural decisions where outside input
would help. Specifically:

### Q1. COP0 modeling — how would you do HP1?

We want the cleanest mapping of `mtc0`/`mfc0`/`rfe` and the SR/Cause/
EPC registers into generated C. Current preference: function-call-
per-access so the runtime can hook SR writes for IRQ delivery. Is
there a better model? How does N64Recomp handle COP0?

### Q2. Self-modifying boot code — how would you do HP2 and HP3?

The BIOS copies kernel code from ROM to RAM during init and runs the
RAM copy. We're inclined to teach the recompiler to recognize the
specific ROM->RAM copy pattern and statically resolve the RAM
addresses to the same generated functions. Is there a more general
solution that doesn't require BIOS-specific pattern recognition?

### Q3. A0/B0/C0 vector dispatch — how to express runtime tables?

The standard PS1 BIOS call pattern is `jal 0xA0` (or `jal 0xB0`, or
`jal 0xC0`) with a function index in `$t1`. The targets at `0xA0`,
`0xB0`, `0xC0` are dispatch stubs that index into a table built at
runtime. How do we express "indirect call through a runtime-built
function pointer table" in generated C, in a way that lets the C
compiler optimize the common case?

### Q4. Should the runtime have any C-side BIOS state at all?

Our current plan: zero C-side BIOS state. The recompiled BIOS owns
the kernel data area in PS1 RAM, the runtime only simulates hardware.
But this means the runtime's debug server can only inspect kernel
state by reading the PS1 RAM array — it has no native pointers to
"the EvCB table" or "the current thread". Is that going to make
debugging miserable, or is it fine?

### Q5. How aggressive should we be about salvaging the v3 hardware sims?

The v3 hardware sim files (`gpu.c`, `dma.c`, `interrupts.c`, etc.)
work and are reasonably well-tested against DuckStation. But every
one of them has at least one branch that handles "if HLE BIOS is
involved, do X differently". We plan to strip those branches as we
audit each file for v4. Is that the right call, or should we rewrite
each from scratch to be sure no HLE assumptions sneak in?

### Q6. Build system

v3 used CMake + Ninja for the runtime and CMake for the recompiler.
v4 inherits the recompiler's CMakeLists.txt as-is. Should the
runtime stay on CMake (for symmetry with the recompiler) or switch
to something simpler given how small the runtime will be in Phase 2?

### Q7. What's the right first compile target?

Phase 1's success criterion is "`gcc -c SCPH1001_full.c` succeeds".
Should we use gcc (MinGW) or MSVC for the eventual full link? v3
used MinGW for the runner and MSVC for DuckStation. v4 has the
freedom to pick. Considerations: debugger quality, link-time
optimization, compile speed for ~hundreds of generated C functions
in one big file.

### Q8. What's the right milestone cadence?

The phase plan above has 6 phases. Each is "weeks of work" with no
intermediate checkpoints described. Should we break this down
finer? Or is the per-phase verification (DuckStation diff) enough?

---

## Project layout

```
F:/Projects/psxrecomp-v4/
├── PLAN.md              <-- you are here
├── CLAUDE.md            in-session rules
├── README.md            short summary
├── bios/
│   └── SCPH1001.BIN     524288 bytes, the recompilation target
├── duckstation/         2.1 GB, modified DuckStation, runtime oracle
│                        (build via cmake; binary at
│                         duckstation/build/bin/duckstation-qt.exe)
└── recompiler/          16 MB, salvaged from v3
    ├── CMakeLists.txt
    ├── src/
    │   ├── main_psx.cpp           kept for Phase 5, not used in Phase 1
    │   ├── ps1_exe_parser.cpp     kept for Phase 5
    │   ├── mips_decoder.cpp       Phase 1
    │   ├── basic_block.cpp        Phase 1
    │   ├── control_flow.cpp       Phase 1
    │   ├── function_analysis.cpp  Phase 1 (needs extension for HP2)
    │   ├── code_generator.cpp     Phase 1 (needs extension for HP1)
    │   ├── annotations.cpp        Phase 1
    │   └── annotations.hpp
    ├── include/
    │   ├── basic_block.h
    │   ├── code_generator.h
    │   ├── control_flow.h
    │   ├── function_analysis.h
    │   ├── gte.h
    │   ├── mips_decoder.h
    │   └── ps1_exe_parser.h
    ├── tests/
    │   ├── l1_decoder_test.cpp   Decoder conformance (Rabbitizer oracle)
    │   └── l2_structural_test.cpp  Codegen pattern verification
    └── lib/
        ├── rabbitizer/  MIPS disassembler
        ├── fmt/          C++ string formatting
        └── toml11/       TOML parser
```

---

## What is NOT in v4

- **v3's runner** (`runner/src/`) — 12,852 lines of interpreter + HLE
  + hardware sims with HLE branches. Not copied. Files will be cherry-
  picked for hardware sims in Phase 2 onward, audited individually.
- **v3's TombaRecomp/** — game-build directory. Phase 5 concern.
- **v3's tools/** — ad-hoc Python scripts for v3-specific debugging.
  Most of these are throwaways. The TCP debug client is the only one
  worth porting and that happens in Phase 2.
- **v3's session handoff files** — forbidden by rules anyway.
- **v3's test logs and debug screenshots** — 555 GB of `.log` and
  `.png` files in v3's TombaRecomp/. Already deleted before fork.
- **A MIPS interpreter for the BIOS path.** This is the core
  architectural commitment. There is no interpreter in v4.

---

## Where to ask for help (the actual asks)

If you (the outside reviewer) take only one thing from this document:

**Tell us if Architecture A is the right call for the PS1 BIOS
specifically, or if there's a fundamental reason it can't work and
we should be doing HLE BIOS reimplementation instead.**

Our reasoning for A over HLE:

1. The PS1 BIOS is 512 KB of well-understood MIPS code. It is small
   enough to recompile in full.
2. Other static recomp projects (N64Recomp, SuperMarioWorldRecomp)
   have proven the model on larger ROMs.
3. HLE BIOS reimplementation has been tried at least three times
   on this project and has failed three times. Empirically, HLE keeps
   collapsing into "stub creep".
4. A recompiled BIOS, by construction, *is* the BIOS. It cannot
   diverge from the real thing because it is generated from the
   same bytes.

Counterarguments we're aware of:

1. HLE can be faster than recompilation (no need to faithfully
   simulate every COP0 quirk and self-modifying boot dance).
2. HLE is what `pcsx-redux` does (OpenBIOS) and it works for them.
3. Self-modifying boot code is genuinely hard to handle in static
   recompilation.

What we want from you:

- Validate or reject the architectural call (Architecture A vs HLE).
- If you validate: tell us the dumbest mistake we're about to make
  that we don't know yet.
- If you reject: tell us specifically *why*, with reference to the
  hard problems above (HP1-HP6) — not just "stick with HLE because
  it's easier", because we tried that three times.

---

## Test Infrastructure

Modeled after snesrecomp and gbarecomp test harnesses. Two tiers:

### L1: Decoder Conformance (Rabbitizer Oracle)

Tests that `MipsDecoder::decode()` produces the correct mnemonic for every
instruction in the BIOS ROM's code regions, using Rabbitizer (already a
project dependency) as the oracle.

**Harness:** `recompiler/tests/l1_decoder_test.cpp`
**Build target:** `l1_decoder_test`
**Oracle:** `rabbitizer::InstructionCpu` (standard) / `rabbitizer::InstructionR3000GTE` (GTE commands)

For every 4-byte word in the BIOS code regions:
1. Decode with `MipsDecoder::decode(word, addr)`
2. Decode with Rabbitizer (InstructionCpu for standard, InstructionR3000GTE for COP2 GTE)
3. Compare lowercase mnemonics
4. Bucket failures by `(oracle → ours)` pattern

**Code regions tested:**
- Boot:   0xBFC00000 – 0xBFC0DC60 (ROM 0x00000–0x0DC60)
- Kernel: 0xBFC10000 – 0xBFC16760 (ROM 0x10000–0x16760)
- Shell:  0xBFC18000 – 0xBFC42800 (ROM 0x18000–0x42800)

**Expected known differences:**
- Our decoder returns "GTE" for all GTE commands; Rabbitizer returns specific
  names (RTPS, NCLIP, MVMVA, etc.) — this is a coarsening, not a bug
- Our decoder returns "COP0CO" for unrecognized COP0 CO instructions
- Data words in code regions may decode differently (both decoders produce
  garbage for non-instruction data — these show up as noise in the buckets)

**Run:**
```bash
cd /f/Projects/psxrecomp-v4
PATH=/c/msys64/mingw64/bin:$PATH
cd recompiler && cmake -B build && cmake --build build --target l1_decoder_test && cd ..
./recompiler/build/l1_decoder_test bios/SCPH1001.BIN
```

### L2-structural: Codegen Pattern Verification (StrictTranslator)

Tests that `StrictTranslator::translate()` produces correct C code patterns
for each MIPS instruction form (snesrecomp-style structural matching).

**Harness:** `recompiler/tests/l2_structural_test.cpp`
**Build target:** `l2_structural_test`
**Forms:** 42 instruction forms covering:
- SPECIAL R-type ALU (SLL, SRL, SRA, shifts, ADDU, SUBU, AND, OR, XOR, NOR, SLT, SLTU, ADD with overflow)
- Multiply/divide (MULTU, DIV, DIVU, MFHI, MFLO, MTHI, MTLO)
- Immediate ALU (ADDIU, ADDI, ANDI, ORI, XORI, LUI, SLTI, SLTIU)
- Load/store (LW, SW, LB, LBU, LH, LHU, SB, SH)
- COP0 (MFC0, MTC0)
- Edge cases ($zero writes discarded, NOP)

Each form: decode instruction → translate → verify C code contains expected
register references, operations, and patterns. Catches wrong register
indices, missing operations, and translation gaps.

**Known out-of-scope for strict translator:** SUB (funct 0x22), MULT (funct 0x18),
COP2/GTE, LWC2/SWC2 — these are handled by the full_function_emitter's code path.

**Run:**
```bash
./recompiler/build/l2_structural_test    # summary
./recompiler/build/l2_structural_test -v # show generated C for each form
```

### L2-semantic: Full State Comparison — PLANNED

Future tier: compile generated C, execute with random register states,
compare against reference single-instruction interpreter. Modeled after
gbarecomp's L2 with mGBA oracle. Requires `emit_one_insn` CLI and
CMake-time code generation.

---

## Phase 4 Current Status (2026-04-19)

Phase 4 goal: boot to BIOS shell. Sony logo renders. Shell entry stalls
on VSync(0) because VSync counter (0x80079D9C) never increments.

### Root cause chain (fully diagnosed)

1. VSync counter increments in the kernel exception handler's walk of
   DCB chain #3 (head at `0xA000E014`)
2. Chain #3 is populated by `func_00004B90` (B_table[0x13]), called from
   `FUN_bfc25e8c` → B0:0x13 trampoline → B-table dispatch at 0x5E0
3. B-table dispatch at 0x5E0 is a 7-instruction pattern:
   `lui/addiu/sll/add/lw/jr/nop` — reads B_table[$t1] and jumps to it
4. The trampoline resolver (runtime/src/traps.c) only handled 4-instruction
   patterns. Pattern 4 matched the first two instructions but failed on the
   third (sll instead of jr), causing a silent dispatch miss → B0:0x13
   was no-op'd → chain #3 never populated → VSync counter never ticked

### Fix applied

Added Pattern 5 to trampoline resolver (runtime/src/traps.c):
- Matches the 7-instruction BIOS A0/B0/C0 vector dispatch pattern
- Fixed ADD vs ADDU detection (BIOS uses ADD func=0x20, not ADDU func=0x21)
- Runtime rebuilt with `-DCMAKE_BUILD_TYPE=Release` (was empty → `-O0`)

### Current register state

| Register/Address           | Value      | Meaning                                |
|----------------------------|------------|----------------------------------------|
| SR (COP0.12)               | 0x00000401 | IEc=1, IM[2]=1 (VBlank unmasked)      |
| I_STAT                     | 0x00000001 | VBlank IRQ pending                     |
| I_MASK                     | 0x0000000D | Bits 0,2,3 (VBlank, CDROM, DMA)       |
| DCB chain head #2 (0xE00C) | 0x00006D88 | Populated (InitRCnt)                   |
| DCB chain head #3 (0xE014) | 0x00000000 | **EMPTY — awaiting Pattern 5 verify**  |
| VSync counter (0x80079D9C) | 0x00000000 | Never increments (empty chain #3)      |
| Dispatch table entries     | 1045       |                                        |
| Dispatch misses            | 0          |                                        |

### Active guards/workarounds

- RFE longjmp unwind for nested exception paths
- jr $k0 permitted; IsC + ExitCriticalSection IEc mask fixed
- Shell-copy normalize() in dispatcher: phys [0x30000..0x5AFFF] → 0x1FC18000+offset
- Primary-copy aliases (69 entries) for [0xBFC10000..0xBFC18BEF] → [0x80000500..0x800090EF]
- RAM-write-trace ring on configurable range
- TCP debug server on port 4370 (native), 4371 (DuckStation)
- VBLANK_INTERVAL = 50000 dispatches

### DuckStation oracle enhancements (in working tree, not yet patched)

- `system_reset` — resets console and re-arms all breakpoints
- `mem_break` — CPU write breakpoint on a memory address
- `mem_hit_last` / `mem_hit_clear` — query/clear last write breakpoint hit
- Refactored breakpoint callback into shared `PcBreakCallback` + `RearmAllBreakpoints()`
- Removed pause-on-hit (was freezing event loop and killing TCP access)

### Outstanding issues (priority order)

1. **Verify Pattern 5 at runtime.** Release build compiled but not yet run
   long enough to reach the FUN_bfc25e8c call site. Poll `read_ram 0xE014`.
2. **If chain #3 populates:** VSync counter should tick. VSync(0) stops
   timing out. Shell progresses past logo hold.
3. **Regenerate DuckStation oracle patch.** Changes in working tree need capture:
   `git -C duckstation diff ffb33c281 > tools/duckstation/psxrecomp_oracle.patch`
4. **Monitor for new dispatch misses** as shell enters rendering loop.

### Build & run commands

```bash
# --- native runtime ---
cd /f/Projects/psxrecomp-v4
PATH=/c/msys64/mingw64/bin:$PATH

# Regen after seed changes:
./recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated/ --emit-full \
  recompiler/seeds/phase2_ghidra_seeds.json

# Rebuild (Release):
cd runtime && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && cd ..

# Run (one instance at a time):
taskkill //F //IM psx-runtime.exe 2>/dev/null
./runtime/build/psx-runtime.exe &

# --- duckstation oracle ---
taskkill //F //IM duckstation-qt.exe 2>/dev/null
python3 tools/duckstation/launch.py -bios

# --- test infrastructure ---
cd recompiler && cmake -B build && cmake --build build --target l1_decoder_test && cd ..
./recompiler/build/l1_decoder_test bios/SCPH1001.BIN

# Probe (ncat):
NC='/c/Program Files (x86)/Nmap/ncat'
(printf '{"cmd":"read_ram","addr":"0x0000E014","len":4}\n'; sleep 1) | "$NC" localhost 4370
(printf '{"cmd":"read_ram","addr":"0x80079D9C","len":4}\n'; sleep 1) | "$NC" localhost 4370

# Regenerate DS patch:
git -C duckstation diff ffb33c281 > tools/duckstation/psxrecomp_oracle.patch
```

---

*End of plan. Last updated 2026-04-19.*
