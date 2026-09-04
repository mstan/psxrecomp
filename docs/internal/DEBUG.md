# DEBUG LOOP (EXECUTION CONTRACT)

Follow EXACTLY. No deviation.

---

# RULE 0 — TOOL VALIDATION (FIRST USE)

On the FIRST use of ANY tool:

You MUST validate what it is doing and its output

Required:
1. Cross-check against another source
2. Verify structure AND content
3. Confirm assumptions

Never:
- Trust output blindly
- Assume correctness

If not validated:
-> ALL reasoning is INVALID

---

# LOOP

0. Tool validation (first use only)
1. Sync state (NOT frame number)
2. Dump full state (native + oracle) via TCP debug server
3. Diff bytes
4. Find FIRST divergence
5. Trace writer (function + instruction + call path)
6. Classify (codegen / runner / timing / config)
7. Fix the tool (never generated output)

If ANY step is skipped:
-> STOP and restart

All inspection must use TCP (Phase 2+).
Before TCP exists (Phase 1), use structured JSON artifacts only.

---

# PSX-SPECIFIC INSPECTION TARGETS

When comparing native vs DuckStation oracle:

- CPU registers (32 GPRs, HI, LO, PC, COP0)
- RAM contents at known kernel addresses (PCB, TCB, EvCB tables)
- VRAM contents (byte-for-byte)
- MMIO read/write sequences
- Timer counter values
- Interrupt fire ordering
- DMA transfer sequences
- GPU command streams

---

# CLASSIFICATION

codegen:
- Wrong C emitted by strict_translator
- Fix: recompiler/src/strict_translator.cpp

runner:
- Wrong hardware simulation (MMIO, DMA, IRQ, GPU, timers)
- Fix: runtime/src/*.c

timing:
- Events fire at wrong cycle / wrong order
- Fix: runtime timing logic

config:
- Missing function seed, wrong address alias
- Fix: seed files, address_aliases.json

discovery:
- Function not found, dispatch miss
- Fix: function finder pipeline

NEVER fix generated output. Fix the tool that produced it.

---

# END-TO-END COMPLETION RULE (NON-NEGOTIABLE)

When debugging boot/runtime flows, **never accept an internal milestone as success
unless the full user-visible loop closes.**

Examples of FALSE success:
- "counter reached 0" is NOT success if the shell still retries
- "IRQ fired" is NOT success if the waiting code still loops
- "callback returned" is NOT success if the owning state machine does not advance
- "data structure written" is NOT success if the consumer still rejects it
- "handler installed" is NOT success if the VBlank counter never increments
- "chain head populated" is NOT success if pixels don't reach the screen

Required method:
1. Define full end-to-end success criteria FIRST — what does the USER see when it works?
2. Trace producer AND consumer — follow the data from origin to final consumer
3. Find the first semantic divergence — where does the consumer's view differ from expected?
4. Fix subsystem behaviour — correct the root cause in the producing subsystem
5. Rerun the full flow — verify the entire loop closes, not just the intermediate step
6. Reject any patch that only changes intermediate state without closing the loop

## Stale Hypothesis Rule

If a handoff contains stale claims contradicted by new logs, **update the problem
statement before changing code**. Never continue debugging from an invalidated
hypothesis. If there are contradictions or stale data, STOP and inform the user
immediately before proceeding.

(Seen in the 2026-04-13 VBlank stall: the handoff said "handler is never installed"
but live TCP inspection proved the handler WAS installed at `0x800DFEE0` — the real
gap was one level up, in the kernel root-counter chain head at `*(0xA000E014)`. The
entire static-analysis cascade below that stale claim was wasted effort.)

---

# TOOLING RULE (NON-NEGOTIABLE)

If at ANY point during debugging the TCP debug server cannot answer a question
you need answered — **STOP IMMEDIATELY**. Do not:

- Guess the answer
- Work around it with printf/fprintf
- Proceed with partial information
- Make assumptions based on code reading alone

Instead:

1. **STOP** all current work
2. **INFORM the user** exactly what data is missing and what command is needed
3. **ASK the user** how to proceed (build the command? change approach?)
4. **BUILD the tooling** if approved, then resume

Adding a new command is cheap — the pattern is one handler in `runtime/src/debug_server.c`
(native) and a matching handler in `duckstation/src/core/psxrecomp_debug_server.cpp`
(DuckStation oracle, then regenerate `tools/duckstation/psxrecomp_oracle.patch`).
See [`../TCP_COMMANDS.md`](../TCP_COMMANDS.md) for the protocol.

A wrong answer from missing tooling costs MORE than the time to build the tool.
Every workaround becomes technical debt that hides the real bug longer.

---

# TOOL LIFECYCLE RULE (NON-NEGOTIABLE)

Claude builds, launches, and closes ALL tools. The user NEVER gatekeeps this.

- Build native: `(cd runtime && cmake --build build)` (with `PATH=/c/msys64/mingw64/bin:$PATH`)
- Build DuckStation oracle: `bash tools/duckstation/build.sh`
- Launch native: `./runtime/build/psx-runtime.exe &`
- Launch DuckStation oracle: `./duckstation/build/bin/duckstation-qt.exe -bios -nogui -fastboot &`
- Close / restart: `taskkill //F //IM psx-runtime.exe` or `//IM duckstation-qt.exe`

Do NOT ask the user to launch, close, or restart anything.
Do NOT tell the user what commands to run — just run them.

### Kill-and-rebuild procedure (Windows)

The exe file lock can persist after process exit (AV, delayed handle release).
Always use this sequence:

```bash
# 1. Kill any running instance
taskkill //F //IM psx-runtime.exe 2>/dev/null
taskkill //F //IM duckstation-qt.exe 2>/dev/null

# 2. If linker still fails with "Permission denied", delete the exe first
rm -f runtime/build/psx-runtime.exe

# 3. Rebuild
PATH=/c/msys64/mingw64/bin:$PATH
(cd runtime && cmake --build build -j)

# 4. Relaunch (one instance at a time — memory policy)
./runtime/build/psx-runtime.exe &
```

Never use `C:/msys64/msys2_shell.cmd` — it spawns orphan processes. Use `bash` with
`PATH=/c/msys64/mingw64/bin:$PATH` inline.
