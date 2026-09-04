# Tomba long-run recursion freeze — working doc

Branch: `bug/recursion`. This is the single source of truth for the long-run
freeze investigation and the soak-harness regression that is currently blocking
reproduction. Update it as we learn.

---

## 1. The bug we are actually chasing (the freeze)

**Symptom.** Tomba runs correctly for a long time, then hard-freezes. Intermittent;
*often sooner than 15 minutes* but **not reliable**. The captured specimen is
`_freeze_specimens/tomba_4393_freeze_46440.dmp` (frame 46440).

**Root cause (from the minidump).** A **runaway guest recursion that never unwinds.**
The per-frame game-flow state machine re-enters *itself* across the
**compiled ↔ dirty-RAM-interpreter boundary**. Validated, symbolized cycle:

```
exec_one (main loop 0x8001A51C)
  -> psx_dispatch_game_compiled(0x8001A954)
    -> 0x8001A954 -> 0x8001AC00 -> 0x8001B2B4 -> 0x8001B5A8 -> 0x80046264
      -> dirty_ram_dispatch
        -> psx_dispatch_game_compiled(0x8001A954)   <-- back to the top
```

Repeats ~1443 times -> native **fiber stack overflows** -> stack-depth guard halts
the process (`psx_fatal_halt`, which parks or exits). The state-machine fields
`[+0x4a]/[+0x4c]/[+0x4e]` on the controller object at `0x801FD800` are pinned at
`1/1/1` and never advance — that is the cycle that won't terminate. The ~1443
frames overflow nearly instantly *once the cycle starts*; the "~15m / intermittent"
is how long until the **trigger condition** is met, not how long the recursion runs.

**What it is NOT** (all prior hypotheses, refuted):
- NOT data corruption — `0x8001a954` is not a wild pointer; it exists nowhere in
  RAM as data.
- NOT nested interrupt delivery — `in_exception=0`, exception counts normal.
- NOT a fiber deadlock/livelock.
- Scene id intact, table `DAT_8007c640` intact.

**Why it's slippery to capture.** `g_psx_dispatch_depth` reads only 5 — it is blind
to this, because the 1443 frames are direct `func_X(cpu)` calls it does not count.
The `recent_fn` ring is time-ordered, so it shows only the leaf churning at the trip,
**not** the recursing cycle. **This is the entire reason the native_stack tool
exists** (see §3).

**Suspected trigger.** CD-DMA dirty-page over-marking: `dma.c:695` marks every
DMA'd word executable/dirty and never clears it (463/512 RAM pages dirty at the
freeze), time-correlated with the onset of boundary re-entry.

**Fix territory (NOT yet done).** The interp<->compiled call/return contract
(`dirty_ram_dispatch` / `psx_dispatch_game_compiled`) must **unwind** at the
boundary instead of **nesting**; and/or stop `dma.c:695` marking non-code CD-DMA
data as executable/dirty. Same family as the "wild call contract" recursion bugs
(Bug A/C/D). The fix is in the **recompiler**, never in generated C.

---

## 1a. Confirmed organic capture — frame 50241 (`build-recursion`, 2026-06-17)

Second specimen, captured live by the 4-soak harness + harvest watcher
(`_freeze_specimens/soak_report_20260617_171145.json`). The native_stack tool
worked end-to-end (walked 300000 frames, decoded the cycle); the early-flush was
not even needed. Confirms §1 and adds the guest-level mechanism:

- `reason`: native-stack guard tripped — runaway guest recursion.
- `recursion_func = 0x8004DEE0`. At halt: **`v0 = 0x8004DEE0`, `ra = 0x8004DEE8`**
  (`= v0 + 8`) -> the guest ran a `jalr v0` whose target slot points back into the
  recursing chain. **Indirect call through a jump-table / function-pointer slot**
  — the wild-call family (cf. seesaw `func_8001DFD4`, bogus `v0=0x8001DFF8`), NOT a
  benign loop. `pc = 0x00000000` = the dispatch `pc==0` unwind sentinel.
- Steady-state lap (~23074x, ~13 host frames each):
  `exec_one -> dirty_ram_dispatch -> psx_check_interrupts -> psx_dispatch_impl ->
   func_80046264 -> func_8001B5A8 -> func_8001B2B4 -> func_8001AC00 -> func_8001A954
   -> exec_delay_slot -> psx_dispatch_game_compiled -> exec_one`.
- Dirty pages on the interpreted path: **`0x80063xxx` and `0x8011xxxx` (overlay
  region)**; `last_store_pc = 0x80116A24`; `dirty_block_tail` tail shows
  self-referential dispatch (`target == ra == 0x80116204`).

**Mechanism, restated with evidence:** a guest indirect transfer (jalr through a
jumptable/fp slot) crosses the compiled<->dirty-interp boundary, and the recompiler
implements each crossing as a **nested host call** (`psx_dispatch_game_compiled` /
`dirty_ram_dispatch` / `exec_one`) that never returns. The guest transfer is
jump/tail-style (guest stack does not grow proportionally) but the HOST stack grows
~13 frames/lap -> fiber overflow. CD-DMA dirty-marking (`dma.c:695`) is what forces
those overlay/0x80063xxx pages onto the interpreted path in the first place.

---

## 2. The reproduction harness ("soaks") — and the regression blocking it

**Workflow.** Run **4 instances** in parallel ("Soak A/B/C/D"). Claude launches
them; the user navigates **2 to a New Game** and **2 into Dwarf Forest** (overworld),
then lets all four **idle**. Running 4x in parallel is how we hit the intermittent
freeze fast. Per-instance memcard dirs (`saves_a..d`), **software renderer**, 4:3,
ports 4393-4396 (`tomba_soak_{a,b,c,d}.toml`). Stay off heavy host work while
soaking (a wall-clock starvation watchdog, 4s no-heartbeat -> exit(2), false-trips
under host load).

**The regression — RESOLVED.** It was a **stale build artifact** (`build-soak`),
NOT the tooling and NOT master. `build-soak`'s generated `.obj` were byte-identical
to the good build, but its *runtime* objects predated the session and the link was
incoherent; a clean from-scratch rebuild (`build-recursion` = master + native_stack
tooling + early-flush) does **not** crash on overworld-load (user-confirmed). The
"crash the instant anything loads" symptom is gone. See §4 / §5.

---

## 3. The native_stack tool (the thing that may be causing the regression)

Uncommitted tooling added to `runtime/src/crash_trace.c` (129 lines): a
host-stack walker `append_native_stack()` that recovers the true recursion cycle
(the `recent_fn` ring can't — see §1). It is called **only** from
`psx_crash_trace_dump()`, i.e. only while a crash/fatal/SEH dump is already in
progress. It walks from the faulting/the current `rsp` up to the fiber `StackBase`,
keeps only validated return addresses (value in `.text` preceded by a `call`),
run-length-collapses them, and emits module-relative RVAs + a histogram. Every
stack read is bounded by `VirtualQuery`; symbolize offline with `nm`
(`_freeze_specimens/analyze_named.py`, `decode_report.py`).

**Known hazard (handoff issue #3).** Both `psx_signal_handler` and
`psx_seh_handler` run **on the faulting stack** and call the dump. If the original
fault is the recursion's stack overflow, `append_native_stack` runs on the
already-exhausted stack and can **re-fault** — taking the whole report with it
(silent SIGSEGV, no JSON). As written, the tool meant to capture the overflow can
**destroy the very report it was built for.**

---

## 4. Evidence collected this session (artifact-level)

Compared `TombaRecomp/build-soak` (A, has tooling, crashes) vs
`TombaRecomp/build-soak-e` (B, pure master, healthy). MD5'd all 49 objects in each:
**only two differ** — `crash_trace.c.obj` and `main.cpp.obj`. `main.cpp` is an
AppImage `getenv("APPIMAGE")` change, **inert on Windows**. All generated BIOS/game
code (`SCUS_942.36_*.c.obj`, `SCPH1001_*.c.obj`) is **byte-identical MD5**. The
psxrecomp reflog shows HEAD parked at master `3ba40b0` since 09:34 that day, so A
and B were regen'd off the **same** master tip — **not** a flavor trap, **not**
stale generated code. (Those were the handoff's two hypotheses; both refuted.)

**Open logical tension — RESOLVED.** The tension (`crash_trace.c`'s only new code
runs in the terminating dump path, yet B "got into game") dissolved when a clean
rebuild WITH the tooling did not crash. The crashing `build-soak` was a stale/
incoherent incremental build; neither the tooling nor a master regression was at
fault. The session pivoted from "explain the overworld-load crash" to "capture and
fix the actual freeze" — which we then did (§1a, §7, §8).

---

## 5. Status & plan (updated 2026-06-17, end of session)

**Done this session:**
- Branch `bug/recursion` created; native_stack tooling committed (`d9b6ae5`).
- native_stack tool **hardened** with an early-flush (commit `e63fdee`): the report
  is written to disk BEFORE the risky host-stack walk, so a walk fault can no longer
  destroy it. (Turned out not to be needed for the captures — the walk completed —
  but it's the right robustness fix and unblocks future overflow captures.)
- Overworld-load regression (§2) **resolved** — stale build artifact; clean
  `build-recursion` is healthy.
- Freeze **captured organically** (3 specimens: A frame 50241, C frame 50088, B
  Dwarf Forest) via the 4-soak harness + `harvest_soak.py`. Deterministic: all three
  halt with identical `v0=0x8004DEE0, ra=0x8004DEE8, pc=0`. Both New-Game and
  Dwarf-Forest areas reproduce → universal per-frame code path.
- Root-caused to the interpreter↔compiled boundary (§7) + Ghidra-mapped the guest
  state machine (§8). External review (§9) rates this Hypothesis A; flags a distinct
  Hypothesis C to rule out with an oracle trace (§10).

**Builds in tree (do NOT delete):** `build-recursion` (X = master+tooling+early-flush,
the good build), `build-soak`/`build-soak-e` (the A/B subjects). Soak configs
`tomba_soak_{a,b,c,d}.toml` (ports 4393-6, software, per-instance `saves_a..d`
seeded from the real card). `harvest_soak.py` watches the shared
`psx_last_run_report.json` and decodes with `allsyms.txt`.

**Next (in order):**
1. **A-vs-C static check:** disassemble the dynamic routine around `0x80063864`
   (the dirty page `func_80046264` first calls into), specifically the instructions
   before/after its transfer to `0x8001A954` and its stack-restoration / intended
   return path (§13). Is the re-entry a legit guest tail-transfer (A) or an interp
   artifact (C)? `0x80063xxx` is runtime-loaded — read from a PARKED instance via the
   debug server `read`, not the static EXE.
2. **Oracle disambiguation (§10):** trace control flow at `0x8001A954` / `0x80046264`
   / `0x80063864` / the interpreted `jal 0x8001A954` vs psxref @ 4380. Strongest
   metric: `main_loop_entries_per_vblank`. Cheap pre-check: on the oracle, sample
   `state[0x4a]` + `0x8009BCDD` over minutes of play — if they sit unchanged for
   thousands of frames, Hypothesis B is falsified outright.
3. **Implement the single mixed-dispatch-owner fix (§11)** — RUNTIME-only
   (`dirty_ram_interp.c` + `cpu_state.h`), so **rebuild, no regen**. Re-soak all 4
   (New-Game area ≈ 14-min repro) to validate the freeze is gone.
4. **Separately:** tighten CD-DMA dirty-marking (§12) — not the root cause but it
   makes the broken boundary common.

No stubs, no HLE, never edit generated C; the fix is in the recompiler/runtime.

---

## 6. Build & run (this project)

```
cd ../TombaRecomp
export PATH=/c/msys64/mingw64/bin:$PATH TMP=/c/msys64/tmp TEMP=/c/msys64/tmp
# regen tools + BIOS + game (generated/ is gitignored; off master tip):
cmake --build ../psxrecomp/recompiler/build --target psxrecomp-game psxrecomp-bios -j8
(cd ../psxrecomp && ./recompiler/build/psxrecomp-bios.exe --config bios/SCPH1001.toml)
../psxrecomp/recompiler/build/psxrecomp-game.exe --config tomba_soak_e.toml
# build:
cmake -S . -B <build-dir> -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON -DPSX_RECOMP_UI=OFF \
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe
cmake --build <build-dir> --target psx-runtime -j8
# run (per-instance card; taskkill first):
taskkill //F //IM psx-runtime.exe 2>/dev/null
nohup ./<build-dir>/psx-runtime.exe --game tomba_soak_a.toml > _soak_a.log 2>&1 &
# decode a freeze report's native_stack:
nm -n <build-dir>/psx-runtime.exe | awk '$2~/^[Tt]$/{print $1,$3}' > ../_freeze_specimens/allsyms.txt
python ../_freeze_specimens/decode_report.py psx_last_run_report.json ../_freeze_specimens/allsyms.txt
```

**Hard rules:** fix the recompiler, never generated C; no printf/log files (TCP
debug server + always-on rings; read `psx_last_run_report.json` for fatals);
per-instance memcard dir per concurrent instance (sharing corrupts the card ->
fake "regression"); software renderer for soaks; `taskkill //F //IM psx-runtime.exe`
before any launch.

---

## 7. The runtime contract — why the host stack leaks (the mechanism)

The codebase ALREADY solves tail-calls for **compiled** code, via two pieces:

- **`psx_dispatch_impl`** (generated; emitted in `recompiler/src/full_function_emitter.cpp:1103`)
  is a **tail-call trampoline**:
  ```c
  for (;;) {
      cpu->pc = 0;
      dispatch(addr);                 // run one compiled func, or interp a dirty block
      if (g_psx_call_bail) { ...resolve/propagate/flatten... }
      if (cpu->pc == 0) { ...contract check; return... }   // genuine return
      addr = cpu->pc;                 // TAIL call: re-dispatch in-place, NO host nesting
  }
  ```
  A compiled function signals a tail-call by setting `cpu->pc` and returning; the
  loop re-dispatches without growing the host stack. Compiled tail-call loops stay flat.

- **`psx_call_contract`** (`runtime/include/cpu_state.h:132`, static inline) is the
  wild-return unwind. After a direct call returns, if guest `$sp`/`$ra` don't match
  the call site, it raises `g_psx_call_bail = 1`, sets `cpu->pc = $ra` (the true
  target), and unwinds frame-by-frame until the frame whose `(ra,sp)` matches
  resolves it (clears bail, resumes).

**The dirty-RAM interpreter BYPASSES both.** In `runtime/src/dirty_ram_interp.c`,
`exec_one`:
- guest `JR`/`J` (case 0x08 ~L550, 0x02 ~L697): correctly unwind — `cpu->pc=target; return 1`.
- guest `JAL`/`JALR` (case 0x03 ~L703, 0x09 ~L556): **NEST** — call
  `psx_dispatch_game_compiled(cpu, target)` directly. The post-call
  `psx_call_contract(...)` is only reached if that nested call RETURNS.
- `dirty_ram_dispatch_inner` (~L1170-1186) does the same for a surfaced compiled
  target: nests `psx_dispatch_game_compiled` instead of returning `cpu->pc=target`
  to the trampoline.

So when interpreted overlay code does `jal func_8001A954` (tail-transfer back into
the per-frame loop), the interpreter nests a host frame. The previous lap's compiled
chain (`func_8001A954 → … → func_80046264`) is still suspended (it called *into* the
interpreter and is waiting for a return that never comes). Every lap leaks the chain;
the `psx_call_contract` that would unwind it is **unreachable** because the nested
`psx_dispatch_game_compiled` never returns (it descends forever).

**Proof it's host-only, not real guest recursion:** at the halt the guest
`sp = 0x801FE338` — only ~7 KB below the stack base `0x801FFFF0` — and it does **not
grow** across all ~23,074 laps. The guest is *iterating* (tail dispatch); the host
is *recursing* (~13 frames/lap).

---

## 8. The guest state machine (Ghidra: `SCUS_942.36_no_header`, base `0x80010000`)

Two nested selectors, both pinned at the freeze:

**Selector 1 — `func_8001A954` (per-frame state dispatcher).** Reads
`state[0x4a]` (halfword) from the object pointer `*(0x1F8001D4)` and switches:
`0 → func_8001A9F0`, `1 → func_8001AC00`, `2 → func_8001D2F0`, `3 → func_8001D480`,
else return. Stuck at **`state[0x4a] == 1`** → calls `func_8001AC00` every frame.
It only *reads* the selector; it never writes it. (Adjacent fields
`[+0x4a]/[+0x4c]/[+0x4e]` read `1/1/1` and never advance.)

**Chain:** `func_8001AC00 → func_8001B2B4 → func_8001B5A8 → func_80046264`.

**Selector 2 — `func_80046264`.** Runs a per-frame call sequence (incl. the dirty
hop `jal 0x80063864` first), then jump-table dispatches on a **byte at `0x8009BCDD`**
(16 cases, table at `0x80013C60`, computed `jr v0`). Stuck case →
`0x8004DEE0` stub (`jal 0x8004DFA0; move a0,s0; j 0x8004DF68`). Also at `0x80046340`
it reads a word at `0x8009BCC8` (scene id, reads 0) and one branch calls overlay
`0x8011AF78`. A third byte selector sits at `0x800A539A` inside the `0x8004DEx` stubs.

**`func_8004DFA0` (the stuck case's handler):** a per-object GPU/GTE render routine
— GTE transforms, appends primitives to the ordering table at `*0x1F800164`, loops
over vertices, `jr ra`. Returns normally; advances **no** selector.

So nothing in the hot loop (`func_8001A954`, `func_80046264`, `func_8004DFA0`)
advances either selector. Whatever flips them (an event/input/animation/CD-script
step) lives elsewhere.

---

## 9. External review verdict (fresh-context second opinion)

- **Hypothesis A (interp↔compiled boundary nesting) — overwhelmingly likely.**
  Mechanically exact: flat guest `sp` + host `+~13/lap` + the unreachable contract
  check fully explain "thousands of successful iterations, then overflow."
- **Hypothesis B (HW/event state wedge) — no positive evidence.** `state[0x4a]==1`
  plausibly just means "active gameplay" and can stay 1 the entire session.
  `0x8009BCDD` indexes a **render handler** (`func_8004DFA0`) → looks like an
  object/render-TYPE selector, not a temporal counter; an object using the same
  renderer every frame for its whole lifetime is normal. Adjacent `1/1/1` may be
  flags, not counters. Normal gameplay continuing right up to the guard trip favors
  A (a real wedge would show visible guest-level nonprogress *before* the host stack
  happened to exhaust).
- **Hypothesis C (NEW, must be ruled out) — interp control-transfer bug.** The
  interpreter could mishandle a JAL delay slot / return-PC / a subsequent JR and
  **fabricate** the re-entry of `0x8001A954`. The flat guest stack does NOT rule
  this out. A short oracle PC-trace distinguishes C from A.

Conclusion: fix/neutralize the boundary stack leak (A) before any broad IRQ/event
hunt (B); run the oracle trace to exclude C first.

---

## 10. Oracle disambiguation plan (psxref @ 4380)

**Cheap pre-check (falsifies B without reproducing the freeze):** on the oracle,
sample `state[0x4a]` (via `*(0x1F8001D4)+0x4A`) and the byte at `0x8009BCDD` over
several minutes of ordinary gameplay. If they commonly sit unchanged for thousands
of frames, the "stuck selector" premise is dead.

**Decisive control-flow trace.** At each entry to `0x8001A954`, `0x80046264`,
`0x80063864`, and the interpreted `jal 0x8001A954`, record: `emulated_cycle`,
`vblank_serial`, `pc`, `ra`, `sp`, `*(0x1F8001D4)`, `state[0x4a]`,
`*(uint8*)0x8009BCDD`, `I_STAT`, `I_MASK`. Plus control-transfers only:
`source_pc`, opcode kind (J/JAL/JR/JALR), `target`, `new_ra`, `sp`. Compare a few
hundred laps recomp vs oracle:

| Result | Verdict |
|---|---|
| Same PC/target/RA/SP sequence (incl. repeated `0x8001A954` entries) | **A** — guest valid, only host representation leaks |
| First divergence at JAL/JR target, RA, delay-slot result, or return PC | **C** — interp control-transfer semantics wrong |
| Flow agrees, then a memory/IRQ value differs and only recomp stays in the loop | **B** — HW/event state divergence |

Strongest single metric: **`main_loop_entries_per_vblank`** (same count + flat guest
sp on both ⇒ selectors are irrelevant to the stack bug). Trace the **pointer** at
`0x1F8001D4` too, not just `[ptr+0x4A]` — the object may be replaced and each
replacement may start in state 1.

---

## 11. Fix design (single mixed-dispatch OWNER) — RUNTIME-only, no regen

Core rule: **exactly one owner of mixed-mode dispatch per native call chain.**
`exec_one` must never blindly call `psx_dispatch_game_compiled` in a way that can
establish another interp→compiled→interp chain.

Make the bail **typed** and **per-`CPUState`** (not a global bool):
```c
typedef enum { PSX_BAIL_NONE=0, PSX_BAIL_WILD_RETURN, PSX_BAIL_MIXED_TRANSFER,
               PSX_BAIL_EXCEPTION, PSX_BAIL_STOP } PsxBailKind;
// in CPUState: PsxBailKind bail_kind;  bool mixed_dispatch_active;
```
The first `dirty_ram_dispatch` becomes the owner (`mixed_dispatch_active=true`). A
nested boundary crossing does NOT start a new loop — it publishes the target and
unwinds to the owner:
```c
if (cpu->mixed_dispatch_active) { cpu->pc = target;
    cpu->bail_kind = PSX_BAIL_MIXED_TRANSFER; return 1; }
```
The owner re-dispatches in a `for(;;)` loop (compiled or interp), consuming
`PSX_BAIL_MIXED_TRANSFER` to continue and other bail kinds to unwind. Real returns
to the original compiled caller still return normally; native depth stays bounded.

Supporting changes:
- **Externalize interpreter continuation** into an `InterpState` (pc, pending-load
  reg/value/valid, cycles); yield only AFTER fully committing the control-transfer
  instruction + its delay slot.
- **Replace the overloaded `cpu->pc==0` sentinel** with an explicit
  `DispatchResult{kind,target}` where practical — far easier to reason about than
  `pc` + return value + a global bool.

**Regression-prone cases to test** (this is the Bug A/C/D contract area):
JAL/JALR delay-slot exec before yield; JR/JALR target captured before delay-slot
mutates the source reg; JALR with `rd`==`rs` aliasing; MIPS load-delay commitment;
exceptions in a branch delay slot (EPC/BD bit); cycle/event accounting exactly once
across a yield; interrupts arriving while a mixed-dispatch owner is active; guest
returns through KSEG aliases; **wild returns must pass THROUGH the owner, not be
consumed by it**; generated code caching guest regs in native locals across calls;
the `pc==0` overload.

**Long-term (bigger codegen change, not now):** continuation-passing / resumable
compiled frames — a compiled block returns the next guest PC/result and a central
dispatcher chooses compiled vs interp — eliminates host guest-call mirroring
entirely. The single-owner boundary is the lower-risk bridge for the current design.

---

## 12. CD-DMA "dirty/executable" marking (`dma.c:695`) — contributor, not root cause

The CD-ROM DMA loop marks **every** word it writes executable/dirty
(`dirty_ram_mark_executable_range(addr, 4)`). It conflates three properties:
*written* vs *modified-relative-to-compiled-bytes* vs *executed*. CD DMA carries
textures/audio/compressed data, not just code. This is NOT the stack-leak root
cause, but it makes the broken boundary common (page-wide dirtiness forces static
code onto the interpreter when unrelated data shares a page).

Better model (aligns with the overlay-cache work): per-region **write
generation/version** + compiled-code **byte-range + expected hash** + **ever-executed**
flag + dynamic translation cache. On ANY write (CPU/CD-DMA/other DMA/decompressor/
BIOS copy), invalidate only the OVERLAPPING compiled ranges. On dispatch: if a
compiled version exists and bytes/generation match → run compiled; else
interpret/compile-cache this byte-version. "Executable" becomes true on instruction
**fetch**, not on write. Natural end state: `address + content-hash → compiled
specialization`, with CD DMA merely one way a new byte-version arrives.

---

## 13. Immediate next disassembly target

The complete **dynamic routine around `0x80063864`** — the dirty page that
`func_80046264` first calls into — especially the instructions **before and after
its transfer to `0x8001A954`**, plus its stack restoration and intended return path.
This is the static-side A-vs-C discriminator: is the re-entry of `0x8001A954` a
legitimate guest tail-transfer (A), or does the interpreter fabricate it via a
mishandled delay-slot / RA / JR (C)?

`0x80063xxx` is runtime-loaded/dirty, so it may NOT match the static EXE — read it
from a **parked instance** (`python tools/debug_client.py --port 4393 read 0x80063864 <len>`,
length in DECIMAL) after re-soaking to a freeze, or from a captured overlay dump.
Earlier parked read of `0x800638C4` showed a clean GTE-loader leaf
(`lw t0..t2; mtc2 ×3; jr ra`), so at least part of `0x80063xxx` is well-formed code.

---

## 15. Strategy reset — control-flow flight recorder (session 2 end; ChatGPT-assisted)

The oracle RAM-diff of two **separately-navigated** idle emulators was a dead end
(alignment noise; §14). New plan (external review): **stop diffing RAM; build a
triggered control-flow flight recorder on OUR side, find the edge that flips at
~frame 50k, then walk a backward causal slice.** Use the oracle only at the END,
for a small causal address set (not whole-RAM).

**My analytical narrowing (reason first, then confirm with the recorder):**
- The 23k laps are **WITHIN ONE FRAME**, not accumulated across frames: the host
  call stack resets to the SDL frame loop every frame (the per-frame dispatch
  returns, then present, then next frame), so host depth CANNOT accumulate across
  frames. So a single frame's per-frame-update spirals to 23k and never returns.
  > **❌ FALSE — superseded by §17.** There is NO per-frame return to a shallow loop:
  > present/`s_frame_count++` is a `gpu_vblank_tick`→`sdl_vblank_present` callback
  > fired *from within* the guest stack, so host depth CAN accumulate across frames.
  > This premise was load-bearing for the whole "frame-50k trigger" framing; see §17.
- The trip cycle advances `psx_advance_cycles(1)` + `psx_check_interrupts` **per
  lap**, so 23k laps ≈ **23k guest cycles ≪ ~565k cycles/frame** → **no VBlank
  can fire before overflow** (~40µs guest time). So it is NOT "stuck waiting for a
  late interrupt" (scheduler-starvation-wait); it is a **control-flow cycle** — the
  per-frame update re-enters itself. => we are in ChatGPT's "branch/target changes
  abruptly → state/interp control-flow divergence" or "interpreter semantic bug" row.

**Decision tree (which problem do we have):**
| Observation | Diagnosis |
|---|---|
| depth grows gradually across frames; CF + scheduler stable | pure mixed-boundary stack leak |
| 23k laps in ONE frame; cycles/events don't advance | mixed dispatch starves scheduler |
| scheduler advances but a branch/target changes abruptly | state / interp CF divergence |
| branch operands identical but recomp picks a different target | interpreter semantic bug |
| recomp value differs, last-writer = a HW-produced write | hardware emulation bug |
| watermark unwind stops the outer pump (normal exec didn't) | bailout impl incomplete |

**Execution order (focused, NOT another broad diff):**
1. **Per-frame frequency + first-occurrence of the re-entry edge** (interp →
   `0x8001A954`) and the jumptable edge (`0x8004630C` → its `jr` target). Is the
   re-entry **ordinary bounded per-frame behavior that stops TERMINATING at ~50k**,
   or a brand-new edge? (ChatGPT expects at least one is normal rendering behavior.)
2. **Last-writer map** on the values controlling those edges (selector `0x8009BCDD`,
   the jump-table entry, `state[0x4a]`, the loop's termination variable): record
   `{pc, frame, cycle, old, new}` per RAM write; when a load feeds the controlling
   branch, emit a backward slice "edge taken because reg==V loaded from Y last
   written at PC Z." That answers *what changed and why*, vs 900 differing bytes.
3. **Indirect-site state** per branch: `{last_target, first_seen_frame,
   last_change_frame, counts_by_target}` — makes "did `func_80046264` start taking a
   new target at ~50k?" instantly answerable.
4. **Oracle, narrowly:** only after #1–3 name a small address set, ask Beetle
   (write-trace `trace_arm`/`ram_diff`, or a tiny **5-address PC hook** added
   locally inside the Beetle core — much smaller than lockstep) whether real HW
   takes the same edge, how often, and whether it ever transitions the controlling
   value the way we do. Anchor epochs on a once-per-update write (OT-cursor reset /
   game frame counter), local-align (allow ins/del), filter framebuffer/OT/stack/
   audio/RNG, compress repeated identical writes.

**Hooks available (runtime, no regen):** `s_frame_count` (main.cpp:1189),
`psx_cycle_count` (psx_cycles.c), the `interp_enter_compiled` boundary
(dirty_ram_interp.c — the re-entry edge `target==0x8001A954` passes through here),
`g_dirty_ram_flow_log` ring, `crash_trace.c` report dump. First instrument = a
re-entry-edge per-frame count ring dumped on the guard trip.

---

## 16. Flight-recorder runs #1/#2 — the recursion is VARIABLE; use the data-driven rings

Two turbo'd soaks with the per-frame site recorders (commits `2475ee0` → `8cbea2a`,
branch `bug/recursion`). **Turbo works and is organic-safe** (TCP `turbo enabled=1`
→ ~2.4× block-dispatch rate; the freeze is identical, just reached faster).

- **Run #1** (frame 63826, `recursion_func 0x8004DEE0`): the edge I instrumented,
  **interp→`0x8001A954`, is ORDINARY** — exactly **1/frame, every frame, even on the
  crash frame** (`max_per_frame=1`). The cycle counter advances normally per frame.
  So the 23k-deep recursion is NOT that edge.
- **Run #2** (frame 49599, `recursion_func 0x8005FF60`): `dispatch_8001A954` =
  **`max_per_frame=0`, `last_frame=0xFFFFFFFF`** → `dirty_ram_dispatch` was **NEVER
  called with `0x8001A954`**. So `func_8001A954` isn't re-dispatched via
  `dirty_ram_dispatch` either. And **`recursion_func` VARIES** (0x8004DEE0 /
  0x8004DFA0 / 0x8005FF60). => **address-specific counters are the WRONG instrument**;
  the recursing functions change run to run.

**The data-driven cycle (from `dirty_block_tail`, run #2, frame 49599) is the real
picture:** `dirty_ram_dispatch` targets are **overlay blocks `0x8011xxxx`–`0x8013xxxx`
(+`0x800E9xxx`)**, called (the `ra` field) from **main-EXE `0x8002xxxx`–`0x8005xxxx`**
(the `0x8005FF60` cluster). Repeating targets (`0x8012608C`×4, `0x800E9120`×4). So the
runaway is the **per-frame MESSAGE/RENDER overlay system** (main-EXE message handlers
`0x8004–0x8005` ↔ interpreted overlays `0x8011–0x8013`), and *which* functions recurse
depends on which message/state is rendering when it trips. The `func_8001A954` chain
from the minidump (§1) was just one instance of this family.

**BUG fixed:** the 3 site recorders (256 entries × 3) overflowed `crash_trace.c`'s 8KB
`re954[]` sub-buffer → truncated the whole report JSON. `SITE_CAP` 256→32 (committed)
so reports parse. **But the recorders are at the wrong sites anyway.**

**PIVOT for next session — stop guessing addresses:**
1. Re-soak with the fixed buffer (turbo'd) and read the **existing data-driven rings
   from a clean report**: `native_stack` (the host cycle), `dirty_block_tail` (the
   `dirty_ram_dispatch` `target`+`ra` cycle — the `ra` names the re-dispatch caller),
   `dispatch_tail`. These show the ACTUAL cycle without guessing.
2. From the `ra` callers (main-EXE `0x8002–0x8005`), Ghidra those functions to find
   the per-frame loop and its **termination condition**.
3. Add the **last-writer map** (hook `psx_write_word/byte/half` in memory.c — generated
   code writes via `cpu->write_word`→these; writer-PC via... NOTE `g_debug_last_store_pc`
   is NOT set per-store in generated code, count was 0 — need another writer-PC source,
   e.g. a global cpu via `debug_cpu_ptr` (memory.c:252) + its pc) on the controlling
   value → the causal slice "loop didn't terminate because reg==V from addr Y written
   by PC Z." `psx_read_word/half/byte` (memory.c 586/671/727) are free funcs usable in
   the write hook to read state.
4. Determinism check worth doing: does the freeze frame VARY run-to-run (63826 vs 49599)
   or is it input/navigation-dependent? Both runs idled on a dialogue; frames differ.

---

## 14. Fix attempt + CORRECTED understanding (session 2, 2026-06-17 PM)

Implemented + live-tested the single-mixed-dispatch-owner fix (§11) at the interp↔
compiled boundary (commits `f488dc5` → `e59043b`, branch `bug/recursion`). Two
iterations: a nesting-counter trigger that **LEAKED** across the fiber exception
longjmp (`psx_exception_longjmp` never returns → the post-call decrement is skipped
→ the counter crept up → false-tripped on a benign call → froze a render) was
replaced by a host-stack-**USAGE** watermark (`PSX_MIXED_STACK_KB`, default 700; can't
leak). Gated by `PSX_MIXED_OWNER` (default on).

**RESULT — the fix is a SYMPTOM-fix and does NOT make the game playable.** Live 2×2
soak (A/B fix-on, C/D fix-off, `overlay_cache` OFF): **ALL FOUR froze at ~14 min
(~frame 50k), fix on or off.** C/D overflowed (recursion crash, harvest report); A/B
hung "(Not Responding)" (the watermark surfaced into the still-wedged guest state and
the main thread spun). The fix bounds the host stack (no overflow crash) but the guest
still wedges → hang — arguably *worse* for debugging than the crash-with-report.
Keep it gated; default it OFF in a future pass.

**CORRECTION — strike a wrong mid-session assessment.** The freeze is **NOT
dialogue-triggered.** It is the **~14-minute IDLE freeze**, time/frame-based
(~frame 50k), **location-independent** (New Game area, Dwarf Forest, or idling on a
dialogue — all identical; same conclusion we reached for Dwarf Forest earlier). The
screenshots of a frozen dialogue were just *where the game sat idle*. The
`e59043b` commit message claiming it "fires at the first dialogue" is **WRONG**:
`overlay_cache` OFF does not move the trigger earlier — the ~14-min timing is the SAME
with cache on or off, so the trigger is NOT interp-coverage; it is a time/frame-
correlated divergence. (USER correction, authoritative.)

**Root, restated:** at ~frame 50k of idle, our sim diverges from real hardware — which
idles indefinitely, **USER-CONFIRMED** — and tips the per-frame loop
(`func_8001A954` → `func_80046264`) into the boundary recursion. The recursion is the
SYMPTOM; the time-based divergence is the ROOT. Every soak so far was **us-vs-us**; we
have never diffed against real hardware over the idle window.

**NEXT = the oracle.** psxref (`<psxref>`, Beetle PSX, TCP **4380**, shares
`card1.mcd`; see [[psxref_oracle]] / `memory/psxref_oracle.md`). Run BOTH our recomp
and psxref idle (**turbo** if possible, to reach ~frame 50k in well under 14 min wall
clock) and find the **FIRST** state / control-flow divergence around frame ~50k — that
divergence is the bug. Candidate signals to diff (find_first_divergence, emu_read_ram,
emu_trace_addr, emu_step): the guest frame/timer counters, the state object
`*(0x1F8001D4)+0x4A`, the dirty-bitmap growth, IRQ `I_STAT`/`I_MASK`, root-counter
state. Whatever first differs from real HW is the lead. The host-stack fix is a dead
end for *fixing* it (kept only as a robustness/observability aid).

---

## 17. REFRAME — gradual host-stack accumulation, not a "frame-50k trigger" (session 3, 2026-06-17, ChatGPT-assisted via user)

**The §15 premise is architecturally false, and it was load-bearing.** §15 asserts
the 23k laps happen *within one frame* "because the host call stack resets to the
SDL frame loop every frame (the per-frame dispatch returns, then present, then next
frame)." **There is no per-frame return to a shallow main loop.** Verified in the
runtime:

- The guest runs continuously on a **1 MB guest fiber** (`traps.c` —
  `psx_fiber_create(1024*1024, …)`; the BIOS runs each TCB thread on its own fiber).
- `s_frame_count++` / present happen inside `sdl_vblank_present()`, which is a
  **callback** fired by `gpu_vblank_tick()` ← `psx_check_interrupts()`
  (`gpu.c:743`, `interrupts.c:288`) — i.e. *from within* the guest's call stack as a
  side effect of cycle advancement, NOT by unwinding the guest stack back to a loop.
- `psx_check_interrupts` appears **×23,084 inside the recursion histogram itself**
  (§1a / the 205330 capture) — vblanks fire *throughout* the accumulation, not after.

So the frame counter advances 1/frame **while the host fiber stack grows monotonically
and never unwinds.** That is exactly the condition for a **gradual cross-frame leak**:

> **Model A (predicted).** Every game-logic tick (30 Hz), the per-frame message/render
> overlay walk crosses the interp↔compiled boundary, and (per §7) the boundary **nests
> one host call chain that never unwinds**. ~1 leaked chain/tick accumulates on the
> fiber; after ~23k ticks the 1 MB fiber is exhausted and the native-stack guard trips.
> **Frame ~50k is a CAPACITY limit, not a trigger.** Real HW never does this because it
> doesn't mirror the guest call graph onto a host stack.

**Why Model A fits the evidence better than Model W ("a state flips at ~50k"):**

| Evidence | Model A (gradual leak) | Model W (50k trigger) |
|---|---|---|
| frame counter advances 1/frame to the very end | ✅ vblank is a mid-stack callback | ⚠️ needs the whole 23k spiral inside one inter-vblank gap |
| `psx_check_interrupts` ×23,084 *in* the cycle | ✅ vblanks fire all through accumulation | ⚠️ then frames would advance — contradicts "one frame" |
| `recursion_func` **varies** run-to-run (0x8004DEE0 / 0x8004DFA0 / 0x8005FF60 / 0x8001A954) | ✅ steady leak, arbitrary leaf at trip | ❌ a fixed self-recursive fn would be constant |
| wall clock 23,074 × 30 Hz ≈ 12.8 min + boot ≈ 14 min | ✅ exact | — |
| `50,241 − 2×23,074 = 4,093` (≈ leak starts at gameplay) | ✅ ~2 display-fields / leaked chain = 1/tick @ 30 Hz | — |

The `native_stack` block in the reports is a **snapshot at the trip** — both models
yield "23,074 copies on the stack at capture," so it never discriminated them. The
missing measurement was always **host-stack depth as a function of frame.**

**The decisive instrument (built this session, RUNTIME-only — rebuild, no regen):**
an always-on per-vblank sample of `(frame, host_stack_used = TEB StackBase − rsp)` on
the guest fiber, decimated 1-per-128-frames into a 512-entry ring. Read **live** via
the `stack_profile` TCP command (while the game is still responsive — no need to reach
the overflow) and dumped in the crash report (before the early-flush, so it survives a
native_stack-walk fault). Files: `main.cpp` (ring + sampler + `stack_profile_json`,
sampled in `sdl_vblank_present`), `debug_server.c` (`handle_stack_profile`),
`crash_trace.c` (report block). Also added `PSX_FIBER_STACK_KB` (`traps.c`) for the
orthogonal **stack-size sweep**: if the overflow frame scales ~linearly with stack
size (0.5×→~27k, 2×→~96k), accumulation is confirmed by a second independent method.

- **Verdict reading:** `used` climbs ~linearly with frame from ~gameplay-start →
  **Model A**; the slope = leak rate, the x-intercept = leak-start frame. `used` stays
  flat then cliffs at one frame → **Model W**.
- **Boot baseline (validated live):** frames 128–2576 sit flat at 1–6 KB (no gameplay
  loop yet → no leak), confirming the instrument reads the right quantity.

**If Model A holds, the fix is §11 (single mixed-dispatch owner / resumable boundary)**
— but framed correctly: *stop leaking one host chain per frame at the interp↔compiled
boundary*, NOT "find what flips at 50k." The §14 "NEXT = oracle idle-diff to find the
first divergence at ~50k" plan is then **misdirected** — there is no 50k divergence to
find; the divergence is at every boundary crossing from gameplay start. (Oracle still
useful later to confirm real HW takes the same guest edges — Hypothesis C — but it is
not the path to the root.)

**VERDICT (2026-06-17, decisive — Model W, NOT Model A).** Turbo'd idle soak, port
4393, `PSX_MIXED_OWNER=0`. The `stack_profile` curve is **DEAD FLAT**: `used` = 5 KB,
`max_kb` = 7 KB, for **every sample from boot through frame 48,504** (continuous
decimated samples 128,256,…,47488,47616,…,48384 and the per-frame `now`=48504, ALL at
5 KB). Then the frame counter **froze at 48,504** and the native-stack guard tripped
with a **deep recursion present** (`native_stack` ~23,073 laps; `recursion_func`
0x8004DFA0). CPU went to 0 (parked, halt-and-serve).

- **Model A (gradual host-stack leak from ~frame 4093) is REFUTED.** The host stack
  did not grow *at all* over 48,503 frames — zero accumulation. The
  `50241−2×23074=4093` arithmetic was a coincidence.
- **Model W (within-one-frame runaway re-entry) is CONFIRMED.** The per-frame loop
  terminates normally to a ~5 KB depth every frame, until **one** frame (48,504) where
  it re-enters itself ~23,073 deep and overflows the 1 MB fiber *within that single
  frame* — which is why no vblank fires during the spiral (frame counter frozen) and
  the per-vblank sampler can't see the climb (the crash report's `native_stack` walker
  does; the two instruments are complementary, not contradictory). §15's *conclusion*
  ("within one frame") was right; its *premise* (return-based loop) was wrong — the
  loop stays shallow because it genuinely TERMINATES each normal frame, not because of
  a per-frame return to a main loop.
- **Trigger is event/state-driven, not a fixed frame counter:** trip frame varies run
  to run (48,504 / 49,599 / 50,241 / 63,826).
- **Selectors at the freeze are the NORMAL idle steady-state** (read live from the
  parked process): `*(0x1F8001D4)`=0x801FD800, `state[0x4a/4c/4e]`=1/1/1,
  `word@0x8009BCC8`(scene)=0, `byte@0x8009BCDD`(jumptable)=0. So the runaway is **NOT a
  flipped top-level selector** (Hypothesis B weakened, as §9 predicted) — it is the
  canonical §8 cycle `func_8001A954→AC00→B2B4→B5A8→func_80046264→(interp overlay
  crossing)→…→func_8001A954` failing to terminate on one frame.

**Corrected next step (re-points to §9 Hypothesis C / oracle, NOT a leak hunt):** the
loop re-enters via interpreted overlay blocks (`dirty_block_tail`: dirty_ram_dispatch
to 0x800E9120 / 0x8012608C / 0x80124318 / 0x801168F4 … called from main-EXE ra
0x8002–0x8005). The question is whether that re-entry is a **real guest tail-transfer**
(then real HW must escape it via an event we don't deliver — Hyp A/B) or an **interp
control-transfer artifact** (Hyp C — interp mishandles a JAL/JALR/delay-slot/RA in an
overlay crossing and fabricates the re-entry). Decide with (1) an **onset-capture**
instrument that trips EARLY (≈a few thousand laps, before overflow) and dumps the
dirty_ram_dispatch sequence at the *first* bad lap — capturing what STARTS the runaway,
not the 23k-deep steady state that overwrites the ring; and (2) a **narrow oracle
check** (psxref @4380): does real HW execute the same overlay crossing without the
unbounded re-entry? The §11 boundary-resumability fix remains SYMPTOM-only (bounds the
stack; guest still wedges → "(Not Responding)", per §14) — the root is whatever makes
the loop stop terminating on the trigger frame.

The `PSX_FIBER_STACK_KB` sweep is now redundant confirmation only: under Model W a
bigger stack just lets the single-frame spiral go deeper before tripping at the SAME
trigger frame (it would NOT push the freeze to ~96k as a leak would).

---

## 18. REFRAME #2 — it is DIRECT compiled recursion, NOT the interp↔compiled boundary (session 3 cont., 2026-06-17)

Built an always-on boundary control-flow flight recorder (`dirty_ram_interp.c`:
`xprobe_event` at every interp→compiled crossing — JAL/JALR at 888/1042, the
block-loop tail-transfer at 1514, and the dd-site at 1352; per-frame summary ring +
per-crossing detail ring; runtime-armable trip via `xprobe_arm`; `xprobe` TCP cmd;
dumped in the crash report). Reproduced the freeze **3 more times** (frames 44145,
~48504-class, 87176) — deterministic, `recursion_func` 0x8004DEE0/0x8004DFA0.

**The decisive negative result.** Through the overflow, the boundary recorder stayed
**FLAT**: per-frame crossings ~1846 (normal) on the frozen frame, `mixed_depth` ≤ 7,
the detail ring's last activity a tight d=1 overlay loop with flat guest `sp`. My
trip never fired (the native-stack guard did). Yet the native_stack shows the §8
chain `func_8001A954→AC00→B2B4→B5A8→func_80046264` **×23,073** plus the render
cluster. Reconciliation:

> **The recursion is the §8 per-frame chain re-entering ITSELF via the recompiler's
> in-range `jal`→`func_X(cpu)` DIRECT C calls — exactly what the native-stack-guard
> comment describes ("an in-range guest jal emits a DIRECT func_X(cpu) call … grows
> the native stack WITHOUT going through psx_dispatch_impl"). The `interp_enter_compiled`
> / `dirty_ram_dispatch` frames in the native_stack are per-lap hops that RETURN
> (g_mixed_depth balanced ≤7) and/or stale stack values the walker collected — NOT
> the accumulating mechanism. §7's "boundary nesting" was the WRONG layer.**

Proof it's direct compiled recursion: (1) `mixed_depth` (inc/dec around the only two
interp→compiled nesting calls) never grows; (2) guest `sp` flat (0x801FE338) ⇒ the
guest uses TAIL-transfers, not calls; (3) the native-stack guard (a compiled-entry
chokepoint, `debug_server_log_call_entry`) is what trips; (4) `recent_fn` (compiled
func-entry ring) shows the recursing cycle, the boundary recorder does not.

**The recursing functions (recent_fn cycle, leaf→):**
`0x8004DEE0 → 0x8004DFA0 → 0x8005FA60 → 0x8005FF60 → 0x80060194 → 0x8005DFD8
 → [0x8005E1A4 → 0x8005E08C] → …`  `recursion_func` 0x8004DEE0.
- `0x8004DEE0` is a **jump-table case inside `FUN_8004dd14`** (a per-frame RENDER
  LOOP: count at scratchpad `0x1F800240`/`0x1F80025A`, object list at `0x1F800270`/
  `0x1F80022C`, dispatch each object by type byte `lbu 0xA(obj)` through table
  `0x80013F00`/`0x80013F18`; case → `jal 0x8004DFA0` = the GPU/GTE render routine).
  `func_8004DEE0` recompiled = `{ jal 0x8004DFA0; j 0x8004DF68 (loop tail) }`.
- `FUN_8004dd14`'s own loop is a backward `j` (flat). So the recursion is NOT its
  iteration — it is `func_80046264`'s subtree transferring back up to `func_8001A954`
  unboundedly (the §8 chain re-entering), with the render cluster as the leaf where
  the guard happens to trip.

**The 5 answers (capture frame 87176):**
1. first mixed-depth>0 = frame 565 (boot) — but mixed-depth is the wrong layer; it
   never grows. The recursion is compiled direct calls.
2. accumulate vs explode → **EXPLODE within one frame** (per-frame crossings flat
   ~1846 every frame incl. the trip frame; 23k laps inside the single frozen frame).
3. cycles/events during → cycles DO advance (`psx_advance_cycles`/lap → 139,703 in
   the frozen frame ≈ ¼ frame) but **no VBlank** (frame frozen) and `psx_check_interrupts`
   not per-lap → no scheduler progress. A control-flow cycle, NOT scheduler-wait.
4. first anomalous transfer → boundary recorder can't see it (wrong layer); the
   compiled cycle is `0x8004DEE0/DFA0 ↔ 0x8005DFD8/E08C/E1A4/FA60/FF60/60194`.
5. **Diagnosis: a real guest control-flow cycle in the per-frame render path,
   compiled into direct host recursion (guest tail-transfer emitted as a host call;
   guest sp flat, host stack grows).** NOT a gradual boundary leak, NOT scheduler
   starvation, NOT interp fabrication at the boundary.

**Next (no soak needed):** Ghidra `func_80046264` + `func_8001A954` to find the edge
that re-enters `func_8001A954` from `func_80046264`'s subtree, and decide: (a) the
recompiler emits an in-range TAIL-transfer (`j`/`jr`) as a direct `func_X(cpu)` call
(→ recompiler emission fix: tail-transfers must set `cpu->pc` + return to the
trampoline, not nest), vs (b) a genuine guest non-termination (the loop's exit
condition diverges from real HW → narrow oracle check). The right onset instrument
is now at the COMPILED-entry chokepoint (`debug_server_log_call_entry`: per-frame
compiled-entry count + func-entry sequence + early trip), NOT the interp boundary.
The boundary recorder (§18) stays as a proven-negative (rules out the boundary).

---

## 20. FINAL DIAGNOSIS — gradual per-frame host-stack leak (Model A), CONFIRMED (session 3 end, 2026-06-17)

Built the §19 compiled-entry depth profile (`debug_server.c` `ce_sample` in
`psx_native_stack_guard` — sampled at EVERY compiled function entry, the deepest hot
path; per-frame MAX host-stack-used + entry count + deepest func; raw TEB at the trip;
`ce_profile` TCP cmd + crash-report block). It samples the GUEST (main-thread) stack
with NO per-frame reset. It settled everything:

**Captured freeze (frame 44732), TEB SANE (not garbage):**
`base=0x9528000000 dealloc=0x9524000000 sp=0x9524FFFFD0` → 64 MB stack reserve, **48 MB
used at trip**, 16 MB headroom, base>sp>dealloc. The guest runs on the **64 MB
main-thread stack** (NOT a 1 MB fiber); the guard correctly trips at ¼ headroom = 48 MB.

**The trajectory is dead-linear: +1.17 KB/frame from gameplay start** (frame 4453 ≈
1948 KB → frame 44732 ≈ 49152 KB), with **`entries/frame` CONSTANT (~1347)** and the
deepest frame always the render cluster (`0x8004DEE0`/`DFA0`/`0x8005FA60`/`FF60`).
Constant entries + climbing depth ⇒ a **fixed per-frame LEAK** (~one ~13-frame call
chain, ~1.17 KB), not a growing recursion.

> **ROOT: ~1.17 KB of host stack (≈ one §8 per-frame chain, entered through the
> interp→compiled boundary) LEAKS every frame and never unwinds. It accumulates on the
> 64 MB main-thread stack until 48 MB → native-stack guard → freeze (~40k frames ≈
> 14 min). The native_stack's "§8 chain ×23,073" is ~41,000 LEAKED chains (one/frame;
> the walker capped at 300k slots), NOT a within-one-frame spiral.**

This is **ChatGPT's Model A, fully confirmed**, and **§7 was right about the leak site**
(the interp↔compiled boundary). The two mid-session reversals were INSTRUMENT
ARTIFACTS, both now understood:
- **§15/§17 "flat → Model W"** was wrong: `stack_profile` sampled `sdl_vblank_present`,
  which runs on the **scheduler fiber** (flat ~5 KB), not the guest stack.
- **§18 "boundary ruled out"** was wrong: `xprobe` **reset `mixed_depth` per frame**,
  masking the cross-FRAME accumulation (the leak is 1 chain/frame — a per-frame reset
  zeroes exactly the accumulating quantity). The boundary IS the leak; it leaks 1
  chain/frame, not within-one-frame.

Lesson (matches the global ring-buffer rule): sample the RIGHT context, at ABSOLUTE
scale, with NO reset that can hide accumulation. §19 (guest stack, absolute, no reset)
is the trustworthy instrument; §17 (wrong fiber) and §18 (per-frame reset) misled.

**The 5 answers, corrected:** (1) the leak starts at gameplay (~frame 4453), depth>0
immediately; (2) **ACCUMULATE across frames** (linear +1.17 KB/frame), NOT explode;
(3) cycles/events/scheduler all advance normally every frame (frame counter advances —
a slow resource leak, not a control-flow stall); (4) the leaked unit is the §8 chain
`func_8001A954→AC00→B2B4→B5A8→func_80046264` entered via
`interp_enter_compiled`→`psx_dispatch_game_compiled` once/frame and left un-unwound;
(5) **diagnosis: gradual interp↔compiled boundary host-stack leak (~1 chain/frame).**

**FIX (the real one, not the watermark):** the §11 single mixed-dispatch-owner /
resumable-boundary design — when the per-frame compiled chain entered from the
interpreter "returns" by tail-transfer (cpu->pc set / pc==0), the
`interp_enter_compiled`/`psx_dispatch_game_compiled`/`dirty_ram_dispatch` frames MUST
unwind back to the outer `psx_dispatch_impl` trampoline instead of being left on the
stack. The §14 watermark fix was a band-aid (bounds the stack, doesn't stop the leak →
game still wedges). The proper fix makes every interp→compiled crossing resumable so
the host stack stays flat across frames. RUNTIME-only (dirty_ram_interp.c + cpu_state.h);
rebuild, no regen. Validate by re-running the §19 ce_profile soak: the per-frame max_kb
must stay FLAT instead of climbing 1.17 KB/frame.

**NEXT:** pin the exact non-unwinding crossing (one per frame, entering `func_8001A954`)
across `interp_enter_compiled` / the block-loop tail-transfer (line ~1514) /
`dirty_ram_dispatch_inner`, then implement the §11 resumable-owner fix and re-soak under
§19 to confirm the climb is gone.

---

## 21. EXACT crossing pinned + fix attempts (session 3 end, 2026-06-18)

Added an env early-trip (`PSX_STACK_GUARD_KB=N` in `psx_native_stack_guard`) to capture
the leak structure after a few thousand frames instead of the full 48 MB. Self-driven
repro (inject `START`=0xFFF7/release=0xFFFF presses through FMV→New Game→idle;
screenshot-validated — see [[self-drive-tomba-soak]]).

**EXACT leaking crossing (early-trip capture, frame 14461, 6 MB):** the per-frame
re-entry chain is
```
func_80046264 → jal 0x80063864 (the dirty "hop", func_80046264's first call)
  → psx_dispatch_impl → dirty_ram_dispatch → exec_one (interp the 0x80063xxx overlay)
  → jalr func_8001A954 → interp_enter_compiled → psx_dispatch_game_compiled
  → func_8001A954 → AC00 → B2B4 → B5A8 → func_80046264 → jal 0x80063864 → …
```
`psx_dispatch_game_compiled` is a SINGLE-dispatch switch (`func_X(cpu); return 1`), not
a trampoline. So the leak is: **once per frame an interpreted overlay (reached via
`func_80046264`'s dirty hop `jal 0x80063864`) does `jalr func_8001A954`, nesting a host
call that never unwinds** (the per-frame loop never returns). ~1 nest/frame = the
1.17 KB/frame climb. Contributor: `func_8001A954`'s page is CD-DMA-dirty (§12), so it is
routed through `dirty_ram_dispatch`/the interp instead of the flat static table.

**Fix attempt #1 — block-loop tail-transfer surface (line ~1514): RULED OUT.** Changed
the block-loop to SURFACE compiled tail-transfer targets to the trampoline instead of
nesting. Correct for tail-transfers (no return obligation) and kept, but the §19 climb
was UNCHANGED (still +1.17 KB/frame) — the leak is the **call** path (jalr/jal), not the
tail-transfer path. (Empirically confirmed: post-fix early-trip leak unit still
`exec_one → interp_enter_compiled ×5238`.)

**The real fix is delicate (NOT a runtime 2-liner — §11 was optimistic):**
- (A) **Surface compiled CALLS to the owner trampoline.** The interp `jalr/jal` to a
  compiled-game target should set `gpr[31]=return_pc`, `cpu->pc=target`, and return
  (surface) instead of nesting `psx_dispatch_game_compiled`. The owner runs the callee
  flat and re-dispatches `return_pc` on its `jr ra` (the overlay resumes at return_pc —
  functionally identical, flat). **RISK:** doing this for *overlay-native* targets
  (precompiled DLLs that C-return with `pc==0`) reintroduces the dwarf→overworld
  lost-continuation bug (comment `dirty_ram_interp.c` ~903-907). So it must fire ONLY
  for compiled-game targets — which needs a **non-nesting membership test**
  (`psx_dispatch_game_compiled` is a `switch`; add a generated `psx_dispatch_game_has()`
  bsearch → REGEN, or a runtime-maintained set). Validate with §19 (flat) + a full
  playtest incl. dwarf→overworld (the regression this risks).
- (B) **§12 CD-DMA dirty-marking precision.** If `func_8001A954`'s page weren't
  spuriously CD-DMA-dirty, it would dispatch via the flat static table and never hit the
  interp boundary. Stop `dma.c:695` marking non-code CD-DMA words executable/dirty
  (per-range invalidation + content hash). Bigger, but removes the boundary pressure
  generally.

**Recommendation:** implement (A) with the generated membership test, gated to
compiled-game targets only, and validate with §19 + a dwarf→overworld playtest before
trusting it. Do NOT ship a naive all-targets surface (regression). The §19 `ce_profile`
(per-frame max_kb must stay FLAT) is the objective regression check; the self-driven
repro + screenshot validation is the harness.

---

## 22. Fix (A) IMPLEMENTED + VALIDATED INSUFFICIENT — it's a guest CALL-CYCLE (session 3 end, 2026-06-18)

Pinned the exact crossing from the xprobe detail: the leaking entry to `func_8001A954`
is a single **`JAL`, src `0x8001A630`** (the main loop's call site — `func_8001A954`'s
only caller) — once per frame. Implemented (A): surface interp `JAL`/`JALR` to a
game-text target (`psx_game_address_in_text` gate, so overlay-native keeps the nesting
contract → no dwarf→overworld risk) to the owner trampoline instead of nesting
`psx_dispatch_game_compiled`.

**Result (§19, self-driven idle soak): STILL CLIMBS (~0.875 KB/frame, down from 1.17).**
So (A) reduced but did NOT eliminate the leak. **REVERTED** (uncommitted, partial, risky
semantics change). The mechanism is a guest **CALL-CYCLE**: `main_loop(0x8001A51C) →
jal func_8001A954 → … → jal back to main_loop → …`. Surfacing the *interp* side just
relocates the nest to the *compiled* side (the chain's `jal 0x8001A51C` is emitted as a
nesting `psx_dispatch_call`). You cannot fix a call-cycle by surfacing ONE crossing —
the host stack mirrors the guest call graph and the cycle nests at whatever edge isn't
surfaced. Whack-a-mole.

**Crucial nuance:** the leak is ~1 host frame PER FRAME (565k cycles between leaks), NOT
per cycle-iteration — so the per-frame loop MOSTLY returns (flat); exactly ONE call per
frame fails to unwind. Real HW idles forever (flat), so on real HW that loop is balanced
(returns each frame). So either (C) our recomp mishandles ONE specific return per frame
(fixable, contained), or it is the fundamental host-stack-mirrors-guest-calls limitation
that only the §11 continuation-passing redesign (compiled funcs return to a single owner
instead of calling each other) resolves.

**DECISIVE NEXT STEP — the oracle (not more surfacing).** Compare the per-frame return
structure of `main_loop`/`func_8001A954` against psxref @4380: does real HW RETURN from
`func_8001A954` to `0x8001A634` each frame (→ we mishandle a specific return = Hyp C,
contained interp/dispatch fix), or does it genuinely never return (→ §11 redesign)? Both
prior surface attempts (block-loop §20, JAL/JALR §22) are proven dead ends; stop
surfacing single crossings. The §19 `ce_profile` flat-vs-climb remains the objective
check; the self-driven screenshot-validated soak remains the harness.

---

## 23. ORACLE DISAMBIGUATION DONE — VERDICT: contained return-contract fix, NOT §11 (2026-06-18)

Ran the oracle disambiguation (only). Both sides traced **in the identical idle state**
(Tomba New Game, starting village, "Hey, you with the pink hair!" dialogue; object
`*(0x1F8001D4)=0x801FD950`, `state/scene/jumptable` all 0). Artifacts in
`_freeze_specimens/oracle/` (`oracle_chain_filtered.json`, `oracle_state.txt`,
`recomp_allfn_frame.json`, `recomp_ce_profile.json`).

**Tooling built (CLAUDE.md §15 — the oracle had a real gap).** psxref has **no**
instruction/PC trace (`emu_step`/`emu_trace_addr` never existed — the handoff/§10 assumed
them). The right oracle is **`psx-beetle.exe`** (shares the wire protocol; Beetle PSX core
INTERPRETS, so its always-on `g_psxrecomp_fntrace_cb` records **every J/JAL/JR/JALR**
with caller/target/ra/a0/a1/kind). Loads Tomba via `--disc tomba.cue <bios>`. Added **SP
capture** to the fntrace ring on BOTH backends (read `GPR[29]` live) — `beetle_libretro.cpp`
+ `beetle_debug_server.c` and recomp `fntrace.c`/`fntrace.h`/`debug_server.c`; runtime-only,
no regen. Also moved psx-beetle's default debug port **4380 → 4382** (`--port N` override):
4380 collided with psxref AND `cdirecomp/CdiRuntime.exe`, and two LISTENers on one port
silently route connections to the wrong process — that dual-listener was the whole
"oracle keeps dying / unknown cmd" confusion early in the session, NOT a real crash.

**ORACLE (real HW), one `func_8001A954` invocation — perfectly balanced, once/frame:**
```
JAL 0x8001A630 -> 0x8001A954  ra=0x8001A5DC  sp=0x801FE3E0   main_loop calls A954 (ret=0x8001A638)
JAL 0x8001A9B8 -> 0x8001AC00  ra=0x8001A638  sp=0x801FE3C8  ┐
JAL 0x8001AC4C -> 0x8001B2B4  ra=0x8001A9C0  sp=0x801FE3A8  │ nested chain,
JAL 0x8001B3E4 -> 0x8001B5A8  ra=0x8001AC54  sp=0x801FE390  │ SP descends
JAL 0x8001B750 -> 0x80046264  ra=0x8001B73C  sp=0x801FE378  │
JAL 0x80046274 -> 0x80063864  ra=0x8001B758  sp=0x801FE360  ┘ (the dirty hop)
JR  0x8001A9E8 -> 0x8001A638                 sp=0x801FE3E0   A954 RETURNS, SP fully restored
```
- `func_8001A954` is entered **exactly once/frame** (`JAL` from `0x8001A630`) and **returns
  exactly once/frame** (`jr ra` at `0x8001A9E8` → `0x8001A638`), with **SP back to its entry
  value `0x801FE3E0`** ⇒ the whole subtree unwinds every frame.
- **NO re-entry of `func_8001A954` before its exit** (armed on `0x8001A954`; the only hit per
  frame is the `0x8001A630` JAL). **The overlay `0x80063864` NEVER calls back into
  `func_8001A954`** — §21's "interp overlay does `jalr func_8001A954`" is **REFUTED on real
  HW**. There is **no guest call-cycle**: main_loop is a per-frame VSync callback that calls
  A954 and A954 returns; §22's "`jal back to main_loop`" was a misread of the recomp's
  forward-dispatched returns (below).

**RECOMP (`build-recursion`, `PSX_MIXED_OWNER=0`), same idle — IDENTICAL guest flow, but
every boundary-crossing RETURN is a forward `psx_dispatch`:**
```
... render leaf 0x8004DEE0 (ra=0x8004DEE8) ×5 ...
psx_dispatch 0x800462C4  sp=0x801FE360   (80046264 subtree returns)
psx_dispatch 0x8001B758  sp=0x801FE378   (80046264 -> B5A8 return)
psx_dispatch 0x8001B3EC  sp=0x801FE390   (B5A8 -> B2B4 return)
psx_dispatch 0x8001AC54  sp=0x801FE3A8   (B2B4 -> AC00 return)
psx_dispatch 0x8001A9C0  sp=0x801FE3C8   (AC00 -> A954 return)
psx_dispatch 0x8001A638  sp=0x801FE3E0   (A954 -> main_loop return; SP restored)
```
- The **guest** stack unwinds **identically** to the oracle (SP climbs back to `0x801FE3E0`,
  same addresses, same RA). The down-chain calls (A954→AC00→…→80046264) are compiled DIRECT
  calls; the render leaves are interpreted overlays (`0x8011xxxx`/`0x8012xxxx`/`0x8006Bxxx`,
  ra `0x8001E0D8`). When those interpreted/dirty frames **return** to a compiled main-EXE
  address, the dirty-RAM dispatch realizes the `jr ra` as a **forward `psx_dispatch(ret)`**
  that NESTS `psx_dispatch_game_compiled` instead of surfacing to the trampoline owner.
- **Live leak confirmed** (`ce_profile`): `max_kb` 20099→22215 over 1807 frames =
  **+1.17 KB/frame**, `entries/frame` constant ~1300, deepest func the render cluster
  (`0x8004DEE0`/`0x8005FF60`/`0x8005FA60`). Identical to §20.

**VERDICT (decisive):**
1. **No guest control-flow divergence.** Targets, RA, delay-slot results and SP are
   bit-identical on both sides. ⇒ Hypothesis **C-as-interp-fabrication is RULED OUT** (the
   interp does NOT mishandle a JAL/JALR/delay-slot/RA; it does not fabricate the re-entry).
2. **Beetle UNWINDS each invocation; the recomp leaves the chain SUSPENDED** (ChatGPT
   determination **#1**), because the recomp **maps the guest's nonlocal-but-balanced returns
   onto accumulating host calls** (determination **#2**). The guest **genuinely returns every
   frame** (proven by SP on BOTH sides) → this is a **CONTAINED return/unwind-contract bug**,
   **NOT** the §11 continuation-passing redesign.
3. **The leaking edge is the RETURN path, not the call path.** The boundary crossings that
   leak are the up-chain `jr ra` returns from interpreted/dirty code into compiled main-EXE
   functions: `0x800462C4 / 0x8001B758 / 0x8001B3EC / 0x8001AC54 / 0x8001A9C0 / 0x8001A638`.
   This is **why §20 (block-loop tail-transfer) and §22 (interp JAL/JALR down-calls) failed**
   — they surfaced the CALL path; the leak is the dirty→compiled RETURN dispatch nesting.

**Next (the fix, contained — runtime-only, no regen):** at the dirty-RAM↔compiled boundary,
a guest **return** (`jr ra`, and any dirty-dispatch whose target is a compiled-game address
reached as a RETURN) must **surface `cpu->pc=ret` to the `psx_dispatch_impl` trampoline owner
and UNWIND**, not nest `psx_dispatch_game_compiled`. Single-owner model is §11's *boundary*
piece (NOT the full resumable-frame codegen). Regression-guard with: (a) §19 `ce_profile`
per-frame `max_kb` must go **FLAT** (currently +1.17 KB/frame); (b) the oracle trace above is
the gold reference — recomp `psx_dispatch` returns must vanish into host unwinds while the
guest JAL/JR/SP sequence stays bit-identical; (c) full dwarf→overworld playtest (the
overlay-native `pc==0` return contract is the regression risk — the surface must fire for
compiled-GAME return targets, not overlay-native DLL returns).

---

## 24. native_stack capture CORRECTS §23's fix scope — it is the §11 boundary work, NOT a one-liner (2026-06-18)

Before implementing the §23 fix I captured a clean early-trip `native_stack`
(`PSX_STACK_GUARD_KB=6144`, frame 7592, 6 MB; report decoded with `decode_report.py`).
**This revised the fix scope — §23's oracle VERDICT stands, but its "quick contained
runtime patch" framing was wrong.** Recorded honestly:

**The leaking host cycle (native_stack histogram, ~5239× each, ≈1 chain/frame):**
```
func_8001A954 → func_8001AC00 → func_8001B2B4 → func_8001B5A8 → func_80046264
  → psx_dispatch_impl → dirty_ram_dispatch → exec_one
  → interp_enter_compiled → psx_dispatch_game_compiled → func_8001A954  (repeats)
```
So the per-frame chain (the §8 chain, emitted as DIRECT compiled `func_X(cpu)` calls)
**re-enters itself**: `func_80046264`'s dirty hop (`psx_dispatch_impl` → `dirty_ram_dispatch`
→ interp) loops back up to `func_8001A954`, and the direct-call chain never unwinds. This is
exactly the native-stack-guard comment's "in-range guest transfer emitted as a DIRECT
`func_X(cpu)` call grows the native stack WITHOUT going through `psx_dispatch_impl`."

**Instrument cross-check (the instruments DISAGREE — do not over-trust native_stack):**
- `ce_profile` (the ONE trustworthy gauge, §20): **+1.17 KB/frame, real, live-reconfirmed.**
- `xprobe` per-frame summary: `a954`≈1/frame, `cr`≈1844 crossings/frame, **`dmax` (mixed_depth)
  BOUNDED at 6** (reset per frame); the xprobe *detail* ring shows the chain UNWINDING cleanly
  at frame end (mixed_depth d=1, surfaced returns to 0x8001A638/0x8001A9C0/0x8001AC54/…).
- `dirty_insn_log` (65536 insns) and `dirty_block_log`: **NO transfer/dispatch targets
  `0x8001A954`.** So the re-entry is NOT a dirty-interp instruction with a literal
  `func_8001A954` target, and `dirty_ram_dispatch(0x8001A954)` is never called (matches §16).
- ⇒ The `interp_enter_compiled → psx_dispatch_game_compiled → func_8001A954` frames in the
  native_stack are partly **the compiled chain's own direct calls** (and possibly some stale
  return addresses the walker over-collects). The accumulating unit is the COMPILED per-frame
  chain not unwinding — the interp boundary is the *enabler* (CD-DMA dirtiness routes the
  chain through `dirty_ram_dispatch`, §12), not a simple nesting site.

**Corrected fix scope.** §23's oracle proof (real HW's `func_8001A954` is balanced and returns
once/frame, guest SP bit-identical) RULES OUT a simple one-instruction return-contract bug —
but that means the fix is NOT the quick "surface the return" patch §23 sketched. The recomp's
**whole per-frame chain is emitted as direct nested C calls that re-enter via the CD-DMA-dirty
boundary and never unwind**; making them unwind is the **§11 single-mixed-dispatch-owner /
resumable-boundary** work, which spans the interp boundary (`dirty_ram_interp.c`, runtime) AND
the `psx_dispatch_impl` trampoline (`full_function_emitter.cpp`, **codegen → REGEN**). It is
delicate (the Bug A/C/D contract area) and regression-prone — the §14 watermark band-aid of
this already made the wedge "(Not Responding)". This is precisely the change ChatGPT's gate
flagged to undertake only deliberately. Two real, substantial options (NOT one-liners):
- **(§11) Boundary resumability / single owner** — the per-frame compiled chain entered from
  the interp must surface tail-transfers/returns to ONE owner trampoline and re-dispatch flat.
  Spans runtime + codegen (regen). Highest fidelity; highest risk.
- **(§12) CD-DMA dirty-marking precision** — `func_8001A954`'s page is only on the dirty/interp
  path because CD-DMA marks its whole page dirty (textures/audio sharing the page). Make
  dirtiness byte-range/content-precise so the compiled chain dispatches via the FLAT static
  table (normal C returns) and the boundary cycle never forms. Removes the enabler at the
  source; lower codegen risk but touches `dma.c` + `dirty_ram` granularity.

**Validation gate for EITHER fix (unchanged):** §19 `ce_profile` per-frame `max_kb` FLAT
(not +1.17 KB/frame) + the §23 oracle JAL/JR/SP sequence stays identical + 15-min+ idle soak
with no freeze + dwarf→overworld playtest (regression). Artifacts this session in
`_freeze_specimens/`: `oracle/oracle_chain_filtered.json`, `oracle/recomp_allfn_frame.json`,
the early-trip `psx_last_run_report.json` (native_stack), `_xprobe.json`, `_dirty_insn.json`.

---

## 25. UNIVERSAL FIX = continuation-passing call/return (user-chosen 2026-06-18; perf-loss OK)

User direction: build the **universal** fix (matures the ecosystem; nothing but Tomba is
load-bearing; "right before fast"). That is **§11 done fully = continuation-passing**, not a
§12 mitigation.

**Current call contract (the host-mirroring root), `full_function_emitter.cpp`.** A guest
`jal target` is emitted as a **nested dispatch**:
```c
{ uint32_t _csp = cpu->gpr[29];
  cpu->gpr[31] = return_addr;
  psx_dispatch(cpu, target);                       // NESTS a psx_dispatch_impl frame
  if (psx_call_contract(cpu, return_addr, _csp)) return; }
// continuation runs INLINE here
```
One nested `psx_dispatch_impl` per guest call frame ⇒ the guest call graph mirrors onto the
host stack. Bounded guest depth → bounded host; **unbounded guest re-entry (the §8 per-frame
cycle) → unbounded host nesting → 48 MB → freeze.** `psx_call_contract`/`g_psx_call_bail`
(Bug A/C/D) exist *only* to paper over the mismatch between host C-nesting and guest ra-based
returns — they are a symptom of the mirroring.

**Continuation-passing emission (the fix).** Emit every guest call as a TAIL-TRANSFER, with
the return address registered as a dispatchable continuation:
```c
// jal target:
cpu->gpr[31] = return_addr;     cpu->pc = target; return;
// jalr rd, rs:
{ uint32_t _t = cpu->gpr[rs];   /* capture BEFORE link → rd==rs alias-safe */
  cpu->gpr[rd] = return_addr;   /* if rd != 0 */
  cpu->pc = _t; return; }
```
The trampoline (`psx_dispatch_impl`, already a `for(;;)` tail-dispatch loop) dispatches the
callee; the callee's `jr ra` sets `cpu->pc = return_addr`; the loop dispatches the registered
continuation block. The **whole call/return runs FLAT in one loop**, driven by the guest's own
`ra`/`sp` — exactly like hardware. Each basic block becomes a separate dispatch (the perf cost
the user accepted). `psx_call_contract`/the bail machinery become unnecessary (no host nesting
to mismatch; a wild `jr` just dispatches the wild PC, as HW would jump there).

**Why this is universal.** It makes the recompiler *architecturally* incapable of host-stack
overflow from guest control flow — any game, any re-entrancy depth, real or divergent. §12
(CD-DMA precision) and fixing the specific `func_80046264→func_8001A954` divergence are
per-symptom shortcuts; this is the complete fix.

**Scope (multi-component — staged, validated rollout, NOT a big-bang):**
1. **C emitter** (`full_function_emitter.cpp`) — the 4 call sites (jal/jalr × orphaned-delay +
   pending-delay paths). Tail-transfer + ensure EVERY `return_addr` (addr+8) is a block leader
   AND a `local_continuation` (so interior dispatch routes back in). This is where the leaking
   §8 chain lives → fixes the main-EXE mirroring. **Needs REGEN.**
2. **Dirty interp** (`dirty_ram_interp.c`) — `interp_enter_compiled` must SURFACE a
   compiled-GAME call target (set `gpr[31]=return_pc`, `cpu->pc=target`, return) instead of
   nesting `psx_dispatch_game_compiled` (this is §22's change — which was only PARTIAL because
   the compiled side still nested; with #1 it completes). Gate to game-text targets so
   overlay-native DLLs (which C-return `pc==0`) keep their contract → no dwarf→overworld
   regression. Runtime-only.
3. **sljit backend + overlay compiler** (`compile_overlays.py`) — mirror the contract so
   sljit/overlay-compiled code tail-transfers too (else mixed contracts). Follow-on stage.
4. **Regen** BIOS + game(s); the `psx_call_contract` / bail code can be left inert first, then
   removed once #1–#3 are proven.

**Regression-prone cases** (test): JALR `rd==rs` alias (handled by capture-before-link);
delay-slot side effects committed before the transfer (already emitted inline before the
terminator); `jr $ra` longjmp-returns for exception handlers (`psx_restore_state_escape`) —
keep as-is; install-slot hooks (Rule 18); jump tables; continuation that lands outside the
caller's function (dispatch routes by table — must be a registered entry/continuation
somewhere); guest regs cached in C locals across a call (must be flushed at block boundary —
calls are block terminators, so block-local temps don't span; verify the emitter never caches
across a terminator).

**Validation (the gate):** §19 `ce_profile` per-frame `max_kb` FLAT + BIOS boots to shell +
Tomba boots to gameplay + the §23 oracle JAL/JR/SP sequence stays bit-identical + 15-min idle
soak no-freeze + dwarf→overworld playtest. Safe point to revert to: commit `ae47e69`.

### 25.1 Implementation status (2026-06-18) — Stage 1 emitter done + GATED; boot regression open

DONE (`full_function_emitter.cpp`, gated behind gen-time env `PSX_CPS`):
- All 4 call sites (jal/jalr × orphaned-delay + pending-delay) emit `cpu->pc=target; return;`
  (tail-transfer) under CPS instead of nested `psx_dispatch` + `psx_call_contract`. JALR
  captures the target reg BEFORE the link (rd==rs alias-safe).
- All 3 `jr $ra` return sites publish `cpu->pc = cpu->gpr[31]; return;` under CPS (legacy just
  `return;` and relied on the nested-dispatch C-return). **This was required** — without it the
  BIOS "completed" instantly (returns left `cpu->pc=0` → outermost trampoline exit).
- Every call's return address registered as a dispatchable continuation (local_continuation in
  the caller's function, else `register_cross_function_target`).

**GATING PROVEN SAFE:** regen+build WITHOUT `PSX_CPS` (legacy) boots Tomba normally — the
default codegen is byte-unaffected; CPS is fully opt-in. So this WIP is safe to land.

OPEN (the boot regression, CPS path only): with `PSX_CPS=1`, BIOS regen+build boots `main()`
then exits at frame 0 (`execution completed, PC=0x00000000`). Root cause (high confidence):
in CPS the trampoline must **dispatch** mid-function continuation addresses (a `jr $ra` returns
to a caller's `return_addr`, which the trampoline then dispatches), but the **dispatch table
contains only function ENTRIES** — a return to a mid-function continuation misses the table →
`psx_unknown_dispatch` → `pc=0` → outermost exit. Legacy never needed continuations in the
table (they were reached by C-return from the nested `psx_dispatch`). 

**NEXT (the fix to make CPS boot):** add every registered continuation (local_continuations +
out_cross_targets) to the **dispatch table** (mapping `continuation_addr → containing func`),
so the trampoline routes a returned-to continuation into its function's entry-switch. Find the
dispatch-table emitter (writes `*_dispatch.c`), and include continuations alongside function
entries. Then re-regen `PSX_CPS=1`, boot-test, and run the §25 validation gate (ce_profile flat
+ oracle-identical + 15-min soak + dwarf→overworld). Stages 2 (interp `interp_enter_compiled`
surface) and 3 (sljit/overlay parity) follow once the C-emitter path boots+flattens.

### 25.2 BIOS-only CPS BOOTS — root cause was NOT the dispatch table (2026-06-18)

The §25.1 "continuations not in the dispatch table" hypothesis was **WRONG** — the
continuation-wrapper machinery already adds every continuation to the BIOS dispatch table
(`func_X_cont_Y` entries; 6256 in the legacy build). Verified by inspecting generated
`SCPH1001_dispatch.c`. The real boot regression (frame-0 exit) had two distinct causes, found
by **dumping the always-on fntrace ring on the abnormal `psx_dispatch` return** (added a tail
dump to `main.cpp`; `psx_cps_exit_trace.json`). Tooling fix on the way: fntrace defaults to
record-NOTHING now (`armed_match` returns 0 when nothing armed; the header comment is stale) —
set `PSX_FNTRACE_ARM=all`.

1. **BIOS A0/B0/C0 vector runtime-fallback** (`emit_dispatch`): used nested
   `psx_dispatch_call(target, stop=ra)`. A nested dispatch runs its own trampoline with
   `stop_addr = the caller's return`; when the handler returns there it **zeroes `cpu->pc` and
   C-returns** to the flat main trampoline → the outermost loop sees `pc==0` and exits. In
   legacy this was safe because the caller's `jal gate` was itself a nested dispatch whose
   `psx_call_contract` resumed the continuation; under CPS the caller tail-transferred, so the
   B-call return must flow back to the SAME flat trampoline. **Fix:** under CPS the vector
   fallback **tail-transfers** (`cpu->pc = target; return;` — `$ra` already correct).
2. **`psx_syscall` Enter/ExitCriticalSection** (`traps.c`): set `cpu->pc = 0` (the legacy
   "return via nested dispatch" idiom) → outermost flat trampoline exits. **Fix:** `psx_syscall`
   now returns `int` (0 = void syscall, 1 = transfer); under CPS the emitter (strict_translator)
   emits `if (psx_syscall(...)) return;` so a void syscall **falls through to the inline
   post-syscall code** (the guest's own `jr $ra`), matching real-HW resume at EPC+4. Keeps
   `cpu->pc==0` so legacy + the dirty interp (`return cpu->pc != 0`) are unchanged. Overlay ABI
   bumped 4→5 (callback signature change).

Result: **BIOS reaches the shell under CPS and runs (user-confirmed).** Also fixed: BIOS-only
build was broken (`interp_enter_compiled` called `psx_dispatch_game_compiled` outside the
`PSX_HAS_GAME_DISPATCH` guard) — gated the whole function.

### 25.3 GAME emitter is a SEPARATE codebase — CPS ported there too (2026-06-18, in progress)

KEY CORRECTION to §25.1's staging: the **game** (`SCUS_*`) is generated by
`code_generator.cpp` (via `main_psx.cpp`), NOT `full_function_emitter.cpp`. The previous
session's `PSX_CPS=1` regen left the game **fully legacy** (0 CPS markers, 3406
`psx_call_contract`) — and the §8 leaking chain (`func_8001A954 …`) lives in GAME code, so the
BIOS CPS work alone could not fix the leak. The game emitter uses a different model (one C
function per guest fn, `block_X:` labels + `goto`, calls as DIRECT nested `func_target(cpu)`
calls with inline `goto` continuations — that nesting IS the leak).

Ported CPS to `code_generator.cpp` (gated `PSX_CPS`):
- jal/jalr/jr-$ra/jr-table/branch-split/j-split → **tail-transfer** (`cpu->pc = target; return;`
  with `$ra`/`$rd` set; jalr captures rs before the link, alias-safe). `psx_call_contract` /
  `call_by_address` no longer emitted (0 of each in CPS game).
- Each call's return point registered as a dispatchable **continuation**; an **entry-switch**
  (keyed on `cpu->pc`) is emitted at the top of every function AND every overlapping-alias body
  (`psx_alias_body`, 1549 in Tomba — alias groups route a continuation by calling the first
  alias wrapper with `cpu->pc` set, which overrides the `entry` arg).
- `psx_dispatch_game_compiled` (in `main_psx.cpp`) gains continuation cases
  (`case cont: cpu->pc = cont; func_owner(cpu);`) + entries clear `cpu->pc`. 3340 continuations.

Unified flat trampoline: the BIOS `psx_dispatch_impl` already routes ALL game text through
`dirty_ram_dispatch → psx_dispatch_game_compiled`, so one trampoline drives BIOS + game flat.
Under CPS the game funcs tail-transfer after one block, so the interp→`psx_dispatch_game_compiled`
boundary is depth-1 (no accumulation) → Stage-1b alone should bound the host stack. Mixed
CPS-BIOS + legacy-game gave "black screen after PS logo" (boundary contract mismatch) — expected;
full CPS is the destination.

### 25.4 LEAK FIXED — full-CPS Tomba boots, plays, soak FLAT (2026-06-18) ✅

One more runtime fix was needed past first-boot: **`psx_syscall` case 3 (ChangeThread)**. The
fiber thread-scheduler (`psx_change_thread_fiber`) leaves `cpu->pc == 0` on thread resume
(lines 289/343); legacy resumed via `cpu->pc==0` + the nested-dispatch C-return, but under the
flat CPS trampoline `pc==0` at the outermost level = exit. Tomba exited at **frame 712** on a
ChangeThread for exactly this. Fix: case-3 ChangeThread-success returns **0** (CPS falls through
to the wrapper's own `jr $ra` so the thread resumes at its caller via the flat trampoline);
legacy ignores the return value.

**Result (user-validated):** full-CPS Tomba boots BIOS → FMV → New Game → village idle (the soak
repro spot) and is playable. **The host-stack leak is ELIMINATED:** `ce_profile` `cur_max_kb`
held **FLAT at 5 KB** from frame ~9.9k through **frame 56,890** (turbo soak, ~zero climb) —
legacy climbed +1.17 KB/frame to 20,000+ KB → 48 MB → the ~14-min/~50k-frame freeze. We blew
straight past the freeze threshold with the stack bounded. Branch `bug/recursion`, UNCOMMITTED.

NOTE: process RSS grows under `PSX_FNTRACE_ARM=all` (the 144 MB fntrace ring + turbo buffering);
it decelerates and is unrelated to the (now-fixed) host-stack leak. Run gameplay WITHOUT
`PSX_FNTRACE_ARM` for a normal footprint.

**Validation gate status:** ce_profile FLAT ✅ · 56.9k-frame soak no-freeze ✅ · boots+plays ✅.

### 25.5 Stage 3 — overlay (sljit + gcc-DLL) CPS parity (2026-06-18)

> Historical session log. The sljit tier was removed in `5b7e69b4` (2026-07-15);
> the gcc-DLL findings below remain current, and the toolchain-free role sljit
> filled is now the bundled tcc tier. A gcc miss falls to tcc, then interp.

**sljit JIT (overlay_sljit.c), scoped:** `jr $ra` publishes `cpu->pc`; call-containing
fragments **decline to the interp** (CPS-safe, depth-1); `jr rX` already tail-transferred.
So only leaf shards JIT — `jr` covered, calls interp. Validated: boots, leak flat, shards
persist. A runtime CPS flag `g_psx_cps_mode` (defined in overlay_loader.c, set by a
`__attribute__((constructor))` emitted in the BIOS + game CPS dispatch) gates the sljit/overlay
CPS behavior.

**gcc-DLL path — FULL native overlay coverage under CPS (the production path), DONE + proven:**
- `compile_overlays.py --cps` sets `PSX_CPS` for the recompiler → CPS overlay C (tail-transfer
  + per-function entry-switch; `code_generator` `--overlay` mode only affects widescreen, not
  CPS). The DISPATCH_PREAMBLE's `psx_syscall` decl was fixed void→int. `PSX_OVERLAY_CODEGEN_VER`
  bumped 3→4 (fresh `cg4/` cache namespace; legacy unit-model DLLs in cg3 ignored).
- **Loader continuation routing** (`overlay_loader_dispatch`, gated `g_psx_cps_mode`): under CPS
  an overlay function is flat-dispatched (one block then tail-transfer), so a callee returns to
  a mid-function continuation that idx_head() (entry-keyed) misses. New `overlay_find_by_range()`
  finds the owning candidate by code range and re-enters it with `cpu->pc = addr` → its
  entry-switch routes to the block. gcc DLLs are the trusted tier (diff-validated at entry), run
  native directly. This is the CPS equivalent of the old unit-model that fixed the dwarf→overworld
  blue screen — overlay funcs no longer run as units; continuations re-dispatch flat.
- **Result:** CPS overlay DLLs compile + load + `dispatch_native > 0`; with continuation routing
  **`dispatch_interp_fallback` → 0** (full native overlay coverage), ce_profile FLAT (6 KB),
  boots clean. Addresses are KSEG0-consistent (overlay C sets `gpr[31]=0x800E…` continuations =
  the entry-switch cases).

REMAINING: capture breadth — only the 2 overlays my short run captured are gcc-compiled; other
scenes' overlays gcc-miss → sljit-leaf/interp (still CPS-safe) until captured+compiled (play
through → `overlay_captures.json` grows → re-run `compile_overlays.py --cps`). dwarf→overworld
playtest (the regression check). Then consider CPS default + removing inert
`psx_call_contract`/`g_psx_call_bail`. Commit Stage 3 when the user is satisfied.


### 25.6 CPS IS NOW THE DEFAULT (2026-06-18) — landing on master

The PSX_CPS gate is flipped to DEFAULT-ON in all three emitter sites
(full_function_emitter.cpp ×2, code_generator.cpp, strict_translator.cpp):
`cps = (getenv("PSX_CPS") == nullptr) || (PSX_CPS[0] != '0')`. So a normal regen
emits continuation-passing; `PSX_CPS=0` is the legacy opt-out (escape hatch, no
recompiler rebuild needed). Verified: default regen → CPS markers present; PSX_CPS=0
→ 0 markers; default-CPS Tomba boots, no frame-0 exit, ce_profile flat (4 KB).

Merged to master + pushed (both repos). The shared-framework caveat: ApeEscape/MMX6
pin old psxrecomp commits, so they're unaffected until they bump their pin + regen —
at which point they build CPS (validate then, or PSX_CPS=0 to stay legacy). Overlay
cache codegen_ver=4 (CPS) means production overlay caches rebuild CPS on next compile;
the production game.toml overlay_autocompile_cmd no longer needs --cps (default-on
makes the recompiler emit CPS regardless). The inert psx_call_contract /
g_psx_call_bail machinery is left in place (a future cleanup once CPS has soaked).
