# PSXRecomp v4 — Phase 2 Hand-off (2026-04-11)

> **Audience:** a fresh Claude Code session. Read this FIRST, then
> follow the reading order below.

---

## 1. READ FIRST (in this exact order)

1. `CLAUDE.md` — the project constitution. Re-read in full.
2. `PLAN.md` — Phase 2 spec starts at line ~1017.
3. `PRINCIPLES.md` — debugging discipline (adapted from recomp-template).
4. `DEBUG.md` — debug loop contract.
5. `C:\Users\Matthew\.claude\projects\F--Projects-psxrecomp-v4\memory\MEMORY.md` — auto-memory index.

Then state: "Architecture A is locked. No interpreter. No HLE. No stubs. BIOS first. Game never until Phase 5."

---

## 2. GIT STATE

Repo initialized this session. Single commit:

```
7c40d33 Phase 1 complete: recompiler proven correct on SCPH1001.BIN
```

All Phase 1 work is committed and clean.

---

## 3. PHASE 1 — COMPLETE (do NOT redo)

### Phase 1a — Boot Slice
- `generated/boot_slice.c` — 30 instructions, 0xBFC00000..0xBFC00074
- Compiles clean with `gcc -c -Wall -Wextra`
- Frozen baseline. Do not modify.

### Phase 1b — Instruction Coverage
- 54 opcodes in `recompiler/src/strict_translator.cpp`
- 44,629 walker-reachable instructions, 0 missing
- 2 deferred (COP1/LWC1 — FPU, PS1 has none)
- 2 impl-but-unused (RFE, SRLV)

### Phase 1c — Function Discovery
- 76 functions from 2 seeds (0xBFC00000, 0xBFC00180)
- 2,988 instructions validated through StrictTranslator
- 199 direct call edges, 27 indirect jumps
- Seeds file: `recompiler/seeds/phase1c_seeds.json`
- Artifacts: `generated/function_manifest.json`, `function_edges.json`, `discovery_run.log.json`

### Phase 1d — Indirect Control Flow
- 27 sites: 21 A0/B0/C0 dispatch, 4 JALR, 2 computed tail calls
- 16-instruction context windows captured for each
- Ghidra cross-referenced, 0 disagreements
- Artifacts: `generated/indirect_jumps.json`, `indirect_jump_classes.json`, `indirect_jump_ghidra_xref.json`

### Phase 1e — Relocation
- ONE ROM-to-RAM copy: `FUN_bfc00420`
  - Source: ROM 0xBFC10000..0xBFC18BEF
  - Dest: RAM 0xA0000500..0x800090EF
  - Length: 0x8BF0 (35,824 bytes)
- Verified by both Ghidra (static) and DuckStation (runtime, TCP debug server port 4371)
- 68/69 function entries match byte-for-byte in RAM
- Normalization: `phys = addr & 0x1FFFFFFF; if phys in [0x1FC10000, 0x1FC18BEF]: phys = phys - 0x1FC10000 + 0x00000500`
- Artifacts: `generated/address_aliases.json`, `normalization_rule.md`, `relocation_proofs/primary_copy/proof.json`

---

## 4. PHASE 2 — WHAT TO BUILD

From PLAN.md:

**Goal:** a runtime executable that links the Phase 1 output, calls
into `func_0xBFC00000` (the recompiled reset vector), and runs the
BIOS until it hits its first MMIO access. The MMIO access fatally
errors with a clear message ("BIOS read I_STAT @ 0x1F801070, no
hardware sim available").

**Concrete work:**

1. **Full BIOS recompilation** — the Phase 1c discovery only found 76
   functions from 2 seeds. The full BIOS has 666 Ghidra functions.
   Before building the runtime, the recompiler needs to emit ALL
   functions into `generated/SCPH1001_full.c`. This requires:
   - Walking the full ROM now that Phase 1e relocation is solved
   - Handling the ROM-to-RAM aliases (one C function per normalized address)
   - Emitting a dispatch table (`SCPH1001_dispatch.c`)
   - The strict translator handles every opcode (Phase 1b proved this)

2. **`runtime/src/main.cpp`** — minimal SDL2 window (or even no window
   initially), CPU state allocation, single call into the generated C
   reset vector function.

3. **`runtime/src/memory.c`** — RAM (2 MB), scratchpad (1 KB), BIOS ROM
   region (512 KB from disk). Read/write entry points. MMIO fatal errors.

4. **`runtime/src/cpu_state.h`** — the real CPUState struct (32 GPRs, HI,
   LO, PC, COP0 registers). Replaces the compile-only `generated/cpu_state.h`.

5. **Implement the 4 trap externs** — `psx_syscall`, `psx_break`,
   `psx_arith_overflow`, `psx_unaligned_access`. These are currently
   forward-declared in `generated/cpu_state.h` and intentionally undefined.
   The runtime must provide real definitions.

6. **Link and run.** Capture which MMIO address the BIOS hits first.

**Success criterion:** the runtime starts, executes generated C, and
dies with "no hardware sim for MMIO @ 0x..." at a real BIOS MMIO read.

---

## 5. KEY DESIGN DECISIONS FOR PHASE 2

### The generated C calling convention
Each recompiled function takes a `CPUState*` and returns void:
```c
void func_BFC00000(CPUState* cpu);
```

The strict translator emits code that reads/writes `cpu->gpr[N]`,
`cpu->hi`, `cpu->lo`, `cpu->cop0[N]`, and calls memory access functions
like `cpu->read_word(addr)`, `cpu->write_word(addr, val)`.

Look at `generated/boot_slice.c` and `generated/cpu_state.h` to see
the exact interface the generated code expects.

### Memory access pattern
The generated code calls extern functions for memory access:
- `psx_read_byte(cpu, addr)` / `psx_read_half(cpu, addr)` / `psx_read_word(cpu, addr)`
- `psx_write_byte(cpu, addr, val)` / `psx_write_half(cpu, addr, val)` / `psx_write_word(cpu, addr, val)`

These are declared in `generated/cpu_state.h`. The runtime must implement them,
routing to RAM/ROM/scratchpad/MMIO based on the address.

### MMIO routing
PS1 MMIO lives at physical 0x1F800000-0x1F802000:
- 0x1F800000-0x1F8003FF: scratchpad (1 KB)
- 0x1F801000-0x1F801FFF: hardware registers (I/O ports)
- 0x1F802000-0x1F802FFF: expansion 2

For Phase 2, all MMIO reads/writes should `abort()` with a message
identifying the address. Phase 3 will add real hardware sims.

### Function dispatch for indirect calls
The 27 indirect jump sites need a dispatch mechanism. For Phase 2,
a simple PC-to-function-pointer lookup table suffices. The recompiler
should emit it.

---

## 6. BUILD ENVIRONMENT

```bash
# REQUIRED for every cmake/gcc/psxrecomp-bios invocation:
PATH=/c/msys64/mingw64/bin:$PATH

# Rebuild recompiler:
cd F:/Projects/psxrecomp-v4/recompiler
PATH=/c/msys64/mingw64/bin:$PATH cmake --build build --target psxrecomp-bios

# Run boot slice (Phase 1a regression check):
cd F:/Projects/psxrecomp-v4
PATH=/c/msys64/mingw64/bin:$PATH ./recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated

# Run discovery (Phase 1c):
PATH=/c/msys64/mingw64/bin:$PATH ./recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated --discover recompiler/seeds/phase1c_seeds.json

# Compile boot_slice.c (self-validation):
PATH=/c/msys64/mingw64/bin:$PATH gcc -c -Wall -Wextra -I generated generated/boot_slice.c -o generated/boot_slice.o
```

GCC 15.2.0, CMake via msys2/mingw64, Windows 10, bash shell.

---

## 7. DUCKSTATION

`duckstation/build/bin/duckstation-qt.exe` — modified build with TCP
debug server on port 4371. BIOS at `duckstation/build/bin/bios/SCPH1001.BIN`.

Launch BIOS-only: `./duckstation-qt.exe -bios`

Connect via Python:
```python
import socket, json
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 4371))
s.sendall(b'{"cmd":"ping","id":1}\n')
# Response: {"id":1,"ok":true,"frame":N}
```

Commands: `ping`, `read_ram`, `get_registers`, `gpu_state`, `screenshot`,
`get_frame`, `frame_range`, `frame_timeseries`, `watch/unwatch`, `pause/continue`,
`step`, `dump_ram`, `dma_state`, `irq_state`, `sio_state`, `quit`.

36,000-frame ring buffer. `ncat` available at `/c/Program Files (x86)/Nmap/ncat`.

---

## 8. GHIDRA MCP

Active program: `SCPH1001.BIN`, base `0xBFC00000..0xBFC7FFFF`, 666 functions.
Server auto-starts. Key tools:
- `mcp__ghidra__get_binary_info`
- `mcp__ghidra__get_code` (format: disassembly or decompiler)
- `mcp__ghidra__disassemble_at`
- `mcp__ghidra__analyze_function`
- `mcp__ghidra__xrefs`
- `mcp__ghidra__get_functions` (paginated)

---

## 9. FILES TO READ FOR PHASE 2

Before writing any code, read these to understand the interface:

1. `generated/cpu_state.h` — the compile-only CPUState and extern declarations
2. `generated/boot_slice.c` — example of what generated code looks like
3. `recompiler/src/strict_translator.cpp` — how each opcode emits C
4. `recompiler/src/function_discovery.h` — the discovery pipeline API
5. `recompiler/src/main_bios.cpp` — the recompiler entry point (extend for full BIOS emission)

---

## 10. WHAT NOT TO DO

- Do NOT modify Phase 1 artifacts or the strict translator
- Do NOT write an interpreter
- Do NOT add HLE shims
- Do NOT create stubs (`return 0;` etc.)
- Do NOT use printf/fprintf for debugging — TCP debug server only
- Do NOT load a game ISO or EXE (Phase 5)
- Do NOT bulk-copy v3 runner files (audit one at a time per CLAUDE.md Rule 7)

---

## 11. COMMIT DISCIPLINE

Git repo is initialized. Commit after each meaningful milestone.
Do NOT amend the Phase 1 commit. Create new commits.
