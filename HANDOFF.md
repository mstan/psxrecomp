# PSXRecomp v4 — Session Handoff (Phase 4, mid-session)

Read first, in order:

1. `CLAUDE.md`
2. `PLAN.md`
3. `PRINCIPLES.md`
4. `DEBUG.md`

Then state exactly:

`Architecture A is locked. No interpreter. No HLE. No stubs. BIOS first. Game never until Phase 5.`

---

## Current state (DO NOT REDO — all of this is built and working)

### Infrastructure complete

| Component | File | Status |
|-----------|------|--------|
| SIO controller state machine | `runtime/src/sio.c` | Ported from v3, audited, wired into memory.c |
| Memory card (SIO protocol) | `runtime/src/memcard.c` | Ported from v3, audited, 128KB .mcd files auto-created |
| SDL keyboard → SIO pad | `runtime/src/main.cpp` | Arrows/XZSA/Enter/RShift mapped to PS1 digital pad |
| TCP debug server | `runtime/src/debug_server.c` | Port 4370, 36K-frame ring buffer, 28 commands |
| GPUSTAT poll-triggered vblank | `runtime/src/gpu.c` | Prevents infinite spin in BIOS VSYNC wait loops |

### What boots and runs

- BIOS executes from reset vector (0xBFC00000)
- Sony logo renders (red diamond visible on screen — confirmed via screenshot)
- BIOS reaches shell main loop (confirmed via RA=0xBFC41D18 in diagnostics)
- No dispatch misses, no crashes
- TCP debug server responds to all commands (ping, get_registers, irq_state, read_ram, screenshot, etc.)
- Memory cards initialized with correct PS1 blank format (verified byte-for-byte)
- Controller connected on port 1, keyboard input feeds into SIO

### Known issue: IEc=0 (interrupts disabled)

COP0 SR is always 0x00000404 (IEp=1, IEc=0, IM2=1). The BIOS never enables IEc.

**This blocks:**
- VBlank interrupt delivery → BIOS animation timers don't advance
- The Sony logo → shell transition (which likely needs VBlank counting)
- Any interrupt-driven BIOS behavior (event chain processing)

**What we know:**
- Exception handler IS installed at 0x80000080: `lui $k0,0 / addiu $k0,0x0C80 / jr $k0 / nop` → handler at RAM 0xC80 (ROM 0xBFC10780), IS in dispatch table
- B0 trampoline IS installed at 0xB0: target 0x5E0 (ROM 0xBFC100E0), IS in dispatch table
- i_mask=0x0000000D (VBLANK+CDROM+DMA enabled) — BIOS DID set up the mask
- i_stat=0x00000009 (VBLANK+DMA pending) — interrupts are pending but never delivered
- ExitCriticalSection (func_1FC0D8C0, syscall $a0=2) exists and is recompiled correctly
- The BIOS never calls ExitCriticalSection during the observed execution

**Root cause candidates (investigate in order):**
1. The BIOS boot code path that enables IEc may go through a dispatch miss that was silently nop'd (check if `psx_unknown_dispatch` ever fires during early boot by adding a one-time counter)
2. The BIOS may enable IEc via MTC0 inside a function that has an early `return` due to a recompiler bug (e.g., alignment check bailing out)
3. The RFE at 0xBFC10B14 in the exception handler has a `jr $k0` that may not be emitting `cpu->pc = cpu->gpr[26]` in the generated code — check SCPH1001_full.c line 36416 for this

**Investigation approach (per DEBUG.md):**
1. Use TCP debug server: `{"cmd":"irq_state"}` to confirm IEc
2. Use Ghidra to find the BIOS function that calls ExitCriticalSection during shell init
3. Trace why that function is never reached
4. Fix the root cause (recompiler, dispatch, or traps.c)

---

## Regeneration (idempotent)

```bash
cd F:/Projects/psxrecomp-v4
PATH=/c/msys64/mingw64/bin:$PATH

# Only if recompiler changed:
cd recompiler/build && cmake --build . && cd ../..

# Only if seeds changed:
python3 tools/gen_phase2_seeds.py
recompiler/build/psxrecomp-bios.exe bios/SCPH1001.BIN generated/ \
  --emit-full recompiler/seeds/phase2_ghidra_seeds.json

# Runtime build:
cd runtime/build && cmake --build . && cd ../..

# Run (ONE INSTANCE ONLY — always taskkill first):
taskkill //F //IM psx-runtime.exe 2>/dev/null
cd runtime/build && ./psx-runtime.exe ../../bios/SCPH1001.BIN
```

---

## Hard constraints

- No HLE behavior anywhere
- No stubs
- No direct BIOS state mutation
- No printf debugging (use TCP debug server)
- No generated C edits
- ONE runtime instance at a time (taskkill before launching)
- Do not touch unrelated systems unless needed for the IEc fix

---

## Next milestone

Fix IEc=0 so VBlank interrupts fire → BIOS logo animation completes → BIOS shell renders with text → controller input navigates shell → memory card screen works.

Phase 4 is complete ONLY when all of that works with no fake events.
