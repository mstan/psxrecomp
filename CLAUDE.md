# PSXRecomp v2

**Goal**: Generic PS1 static recompiler — any PS1 game → native PC binary.

**MVP target**: Tomba! — get it running to the main menu. That proves the toolchain works and builds
the shared foundation (MIPS→C emitter, BIOS stubs, runner) that makes every subsequent game easier.

This is NOT a PS1 emulator. We are NOT cycle-accurate. We are NOT exhaustively documenting functions.
We are translating MIPS machine code to C, compiling it, and running it on the PC. That's it.

Tomba is the vehicle. The recompiler is the product.

---

## ██████████████████████████████████████████████████
## ██  RULE 0: NO GHIDRA = NO ACTION. FULL STOP.  ██
## ██████████████████████████████████████████████████

**At the start of EVERY session, before touching ANY file:**

Call `mcp__ghidra__get_program_info`. If it does not respond:

> GHIDRA IS NOT RUNNING.
> I will not read files, write code, or make any suggestions.
> Load SCUS_942.36_no_header into Ghidra as Raw Binary, MIPS LE 32-bit, base address 0x80010000.
> Start the Ghidra MCP server, reconnect with /mcp, then try again.

**This rule has no exceptions. No "I'll just check the source while Ghidra loads."
No action of any kind until Ghidra responds.**

Before touching any crash address: Ghidra first.
Before implementing any function: Ghidra first.
Before guessing what anything does: Ghidra first.

Ghidra address = PS1 address (no offset — raw binary loaded at 0x80010000).

---

## ██████████████████████████████████████████████████
## ██  RULE 1: NEVER TOUCH generated/tomba_full.c  ██
## ██████████████████████████████████████████████████

`generated/tomba_full.c` is a BUILD ARTIFACT. It is output from the recompiler.

**NEVER read it whole. NEVER modify it. NEVER patch it.**

If something is wrong in the generated code → fix `recompiler/src/code_generator.cpp` and regenerate.

`tomba_full.c` is ~11MB / 200K+ lines. Reading it whole **destroys the context window**.
- Need to find a function? `Grep for the address`
- Need to read it? `Read with offset + limit`
- Need to change it? **You don't. Fix the recompiler.**

---

## The Loop (this is the entire development methodology)

```
1. BUILD runner            →  runner + generated/tomba_full.c → PSXRecompGame.exe
2. RUN game (timed)        →  start, wait 30s, kill
3. OBSERVE visual output   →  Read C:/temp/game_screenshot.bmp  (auto-saved every 300 frames)
                               Compare against src/main.jpg (the reference)
4. OBSERVE log output      →  cat /c/temp/game_output.txt | tail -50
5. IDENTIFY bug            →  wrong colors → shader; wrong geometry → GPU decode; crash → Ghidra
6. GHIDRA if needed        →  understand what the PS1 code actually does
7. FIX the bug             →  opengl_renderer.cpp / gpu_interpreter.cpp / runtime.c
8. GOTO 1
```

## Debugging Hierarchy (DO NOT SKIP STEPS)

When investigating a bug, use tools in THIS ORDER. Do not jump ahead.

**Step 1 — Ghidra any unknown compiled function immediately.**
Compiled range = 0x80010000–0x80097FFF. Call `mcp__ghidra__get_code` before reading source or
adding any printf. Do not guess. Do not read large sections of generated code.

**Step 2 — Use INTERP-CALL trace to find what overlay code calls.**
Overlay functions (≥0x80098000) cannot be decompiled in Ghidra. But every JAL they make to
compiled code shows up in INTERP-CALL. Grep script_output.txt for `[INTERP-CALL]`, then
Ghidra every unknown compiled callee. This is the correct tool for overlay debugging.

**Step 3 — Use PCSX-Redux as ground truth for behavioral questions.**
"What value should entity+0x24 have?" → ask PCSX, not yourself.
Use `mcp__pcsx-redux__pcsx_exec_lua` to read RAM directly during gameplay.
PCSX save state load via MCP is broken — start PCSX fresh, navigate manually.

**Step 4 — Add targeted printf traces as a last resort.**
Log ONE specific value you need. Not hex dumps. Not 512 bytes of overlay code.
If you're adding more than one new printf per investigation cycle, stop and use Ghidra instead.

**NEVER do this:**
- Dump raw overlay bytes and manually decode MIPS instructions by hand
- Dump 512+ bytes to "see what's there" — use PCSX Lua reads instead
- Add multiple rounds of hex dumps hoping to find the bug by exhaustion

Session resume after context clear: **say "Run the game."** That's the entire prompt.
The screenshot + Ghidra are the source of truth. There is no other state to reconstruct.

---

## Visual Debugging

The game auto-saves screenshots every 300 PS1 frames (~5 seconds at 60Hz).

| File | Contents |
|------|----------|
| `C:/temp/game_shot_01.png` .. `game_shot_10.png` | Numbered screenshots every 300 frames (~5 sec) |
| `C:/temp/game_vram.png` | Full 1024×512 PS1 VRAM at frame 300 — textures, CLUTs, framebuffers |
| `src/main.jpg` | **Reference** — what the main menu should look like |

**How to observe output yourself:**
```bash
# 1. Kill any running instance
powershell.exe -NoProfile -File "C:/temp/kill_game.ps1"
# 2. Start game in background
powershell.exe -NoProfile -File "C:/temp/run_in_console.ps1" -bat "C:/temp/psxrecomp_run_game.bat" &
# 3. Wait for screenshots to accumulate
sleep 30
# 4. Kill game
powershell.exe -NoProfile -File "C:/temp/kill_game.ps1"
# 5. Read screenshots (use Read tool — PNG files, small enough to view)
# Read: C:/temp/game_shot_01.png  (frame 300, ~5 sec)
# Read: C:/temp/game_shot_05.png  (frame 1500, ~25 sec)
# Read: C:/temp/game_vram.png     (full VRAM at frame 300)
```

**Compare:** Read `C:/temp/game_screenshot.bmp` and `src/main.jpg` side by side.
- Reference: blue sky, green grass, colorful "TOMBA!" logo, "NEW GAME / LOAD" text
- Wrong colors mean shader/CLUT bug
- Wrong geometry means GPU decode bug
- Missing content means VRAM upload or OT decode bug

---

## Build Commands

Build system is **CMake/Ninja** (NOT MSBuild). Use the C:\temp bat scripts:

```bash
# Build runner (most common — after opengl_renderer.cpp / gpu_interpreter.cpp / runtime.c changes)
powershell.exe -NoProfile -File "C:/temp/run_in_console.ps1" -bat "C:/temp/psxrecomp_build_runner.bat"
cat /c/temp/build_runner_log.txt | tail -10

# Build recompiler (after code_generator.cpp changes)
powershell.exe -NoProfile -File "C:/temp/run_in_console.ps1" -bat "C:/temp/psxrecomp_build_recompiler.bat"
cat /c/temp/build_recompiler_log.txt | tail -10

# Regenerate tomba_full.c (after recompiler changes)
/f/Projects/psxrecomp-v2/build/recompiler/PSXRecomp.exe /f/Projects/psxrecomp-v2/isos/SCUS_942.36

# Run game (timed — kill after 30s then read screenshot)
powershell.exe -NoProfile -File "C:/temp/kill_game.ps1"
powershell.exe -NoProfile -File "C:/temp/run_in_console.ps1" -bat "C:/temp/psxrecomp_run_game.bat" &
sleep 30
powershell.exe -NoProfile -File "C:/temp/kill_game.ps1"
# Then: Read C:/temp/game_screenshot.bmp
```

Key paths: recompiler at `build/recompiler/PSXRecomp.exe`, runner at `build/runner/PSXRecompGame.exe`.

---

## Key Files

| File | Purpose | Touch? |
|------|---------|--------|
| `recompiler/src/code_generator.cpp` | MIPS→C emitter — THE PRODUCT | Yes — this is what we fix |
| `recompiler/src/recompilation.cpp` | Function/branch translator | Yes if needed |
| `runner/src/runtime.c` | Memory map, call_by_address stubs | Yes — add BIOS implementations here |
| `runner/src/main_runner.cpp` | GLFW window, load EXE, run loop | Yes — minimal changes only |
| `generated/tomba_full.c` | **GENERATED. NEVER TOUCH.** | **NEVER** |
| `generated/tomba_dispatch.c` | **GENERATED. NEVER TOUCH.** | **NEVER** |
| `isos/SCUS_942.36_no_header` | Game binary (PS1 EXE, header stripped) | Never |
| `annotations/SCUS_942.36_annotations.csv` | Per-function Ghidra notes → emitted as comments in generated C | Yes — add when a function is confirmed |

---

## Annotation Rule

**When Ghidra confirms what a function does, add it to the annotations CSV.**

File: `annotations/SCUS_942.36_annotations.csv`

Format:
```
0x8001dfd4, entity tick dispatcher — iterates 200 entity slots at 0x800A5970, stride 0xD4
```

The comment appears as `/* [NOTE] ... */` above the function signature in `tomba_full.c`
after the next `PSXRecomp.exe` regeneration. This makes the generated code self-documenting
without ever reading `tomba_full.c` whole.

**When to annotate:** after Ghidra confirms the function's purpose. Not before, not speculatively.
**Annotation scope:** function-level only (one entry per function start address).
**For other games:** create `annotations/<serial>_annotations.csv` — same format, picked up automatically.

---

## Log File Rule

**Every `.c` or `.cpp` file that implements hardware/BIOS behavior gets a sibling `.log` file.**

`runtime.c` → `runtime.log`
`bios_kernel.c` → `bios_kernel.log` (if created)

Log entry format:
```
[function_name]
Ghidra: <what the decompiler showed>
Rationale: <why implemented this way>
```

This is the audit trail. It lives next to the code. It does NOT go in the source file as comments.
It does NOT appear when grepping the source. It is reference only.

---

## Architecture

**This is a static recompiler.** The MIPS binary is translated to C once. The C is compiled to a
native x64 binary. No interpreter loop. No cycle counting. No emulation.

**JAL = direct C function calls.** `func_80012345` calls `func_80067890` directly in C.
`call_by_address()` handles only dynamic jumps (BIOS calls, jump tables).

**runtime.c starts empty.** When the game crashes on an unknown BIOS call, `call_by_address`
logs the address. Look it up in Ghidra. Implement it in `runtime.c`. Log it in `runtime.log`.

**Do not pre-implement anything.** If the game hasn't crashed on it, it doesn't need implementing yet.

---

## What NOT to Do

- Do not pre-emptively implement BIOS functions "just in case"
- Do not read large sections of tomba_full.c for "context"
- Do not guess what a function does — Ghidra it
- Do not add override/patch mechanisms to avoid fixing the recompiler
- Do not carry assumptions from any previous project or conversation
- Do not write verbose documentation — a one-line log entry is enough
- Do not run tests as primary development driver — run the game

---

## Progress Milestones

| Milestone | Status |
|-----------|--------|
| Recompiler generates tomba_full.c | ⬜ |
| Runner links and boots | ⬜ |
| Any GPU packets produced | ⬜ |
| Any pixel visible | ⬜ |
| Main menu visible | ⬜ |
| Main menu interactive | ⬜ |
