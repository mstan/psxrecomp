# COSIM_ORACLE.md — the first-divergence decision procedure

Status: **DESIGN + BUILD IN PROGRESS** (started 2026-06-30). This document is the
durable spec so the plan cannot be lost across handoffs (the way the Beetle-anchored
GPU-draw-time finding was lost — see history at bottom).

## Why this exists

For ~2 weeks the MMX6 cutscene→gameplay wedge has produced a string of confident
"this is it" root causes that each turned out wrong: async-RFE, CD-DMA corruption,
IRQ-storm, block-edge-vs-per-instruction IRQ delivery, load-delay-slot exception
corruption, and (most recently, and still unproven) GPU draw-time backpressure. Every
one was a **single-signal probe with a blind spot** — it saw a slice of the machine
and let us pattern-match a story onto it.

The fix for that failure mode is not another probe. It is a **decision procedure**:
a tool that compares the **complete architectural state** of two backends, stepped
deterministically from boot, and halts at the **first** state that differs. Whatever
it halts on *is* the first divergence — there is no hypothesis that can be "wrong."
It will confirm or kill GPU-backpressure (and everything else) on its own.

**No hypothesis is assumed by this tool.** GPU draw-time backpressure is currently the
leading *unproven* idea (Beetle-anchored, bisect-confirmed as a lever, but never shown
to render a frame). The oracle neither assumes nor privileges it.

## The bug, stated precisely (symptom only)

MMX6, attract boot. The compiled backend's state machine at `0x800CD3F8` advances
main-state `0→1` (skipping FMV substate `6`) around frame ~1638; the FMV never plays
and it wedges black at `02 02`. The dirty-RAM interpreter (`PSX_FORCE_INTERP=1`) plays
the FMV. **Same binary.** Proven: given identical inputs the two backends compute
byte-identically (block + function lockstep, 12.9M + 560K units, zero divergence). So
the divergence is purely **when an interrupt/event is taken relative to the guest
instruction stream** — which then flips an early control-flow branch. Both backends
also over-run vs the Beetle oracle (neither models GPU draw-time), so the *faithful*
defect may be shared and only the phase differs between them.

## Design (conferred with ChatGPT "PSX Static Recompiler Debug", 2026-06-30)

### Architecture: two processes + coordinator (v1)

- Two instances of a **clean, stripped build** (`psx-cosim`), one forced to the
  compiled backend, one to `PSX_FORCE_INTERP`. NO other diagnostic tooling compiled
  in (the existing debug server is laggy — user constraint). Only: the cosim state
  engine + a minimal TCP command server.
- A **coordinator** (Python) drives both over TCP in lockstep and compares.
- Rejected: single machine with full snapshot/restore per block (too slow — 3 MB ×3
  per block; restore-bug-prone). Rejected: full `psx_machine_t` instancing (correct
  long-term, too big a refactor for v1). Two processes gives a trustworthy tool
  soonest because our runtime is global-state-heavy.

### Comparison granularity — cycle-keyed, NOT block-keyed (correctness)

The two backends do NOT share a block structure: compiled hooks per basic block (its
gen-time CFG); the dirty interp hooks per instruction. So a block-leader hash sequence
would not align between them. The ONLY backend-agnostic common clock is
`psx_cycle_count` — both backends call `psx_advance_cycles` with byte-identical
per-instruction charges (proven). So **checkpoint the hash on guest-cycle milestones**
(hook in `psx_advance_cycles`): at cycle stride S both backends are at the same guest
instruction (until the real divergence), so their checkpoints align. The block-leader
hook (`cosim_block(pc)`) is kept ONLY to stash `g_last_leader_pc` for human-readable
reporting (compiled `cpu->pc` is stale mid-block); the compare itself is the
cycle-checkpoint full-state hash. Drill = shrink stride S→1 in the divergent window.

### Stepping + two-level search

1. **Fast mode:** advance both by one block (one dispatch); each returns
   `(seq, leader_pc, state_hash, sub_hashes)`. Coordinator compares. Batch K blocks
   per round-trip and return K hashes to amortize TCP; on a batch mismatch, re-run the
   batch one block at a time.
2. **On first mismatch:** the bug class is "event taken relative to instruction N," so
   a block-level answer isn't enough. Drill: re-run the divergent block at
   **instruction granularity** (per-instruction state hashes) to name the exact
   instruction / control event where they split.

### Determinism (critical — do NOT mask the divergence)

Each process runs its **own** CD/GPU/DMA/timer scheduler from its **own** guest cycle
counter. Both are fed the **same external bytes** (same disc image, same BIOS, same
config, no pad input in attract). Do **not** share one live event schedule between the
machines — that would hide the exact class we're hunting. Each backend reads **live**
devices from its own state (our old lockstep fed *recorded* values and was thus blind
to input divergence — that blind spot is the whole reason for this rebuild).

### Bounded reporting (LLM must not be firehosed — user constraint)

Because the coordinator steps in lockstep and **halts on the first mismatch**, nothing
scrolls away: each process keeps only a **ring of the last N blocks**
`(seq, leader_pc, state_hash, i_stat, i_mask, sr, cycle, irq_taken?)`. On halt, dump
that N-window from both + a full field-level diff of the single divergent step. Query
surface: `divergence` (the one row + diff) and `window?n=…` (the preceding N). Never a
full trace.

## Full architectural state — the completeness checklist

The tool is only as truthful as this list. A missed execution-relevant field = a blind
spot (false "no divergence" or false positive). `boot_state.c` already serializes most
of it; the **gaps below must be added** and the whole thing proven by the gates.

Reuse `boot_state.c` per-module `_snapshot_bytes/write/read` for: RAM(2MB),
scratchpad(1KB), IRQ(i_stat/i_mask), timers, guest clock, GPU regs, VRAM(1MB),
SPU+SPURAM, CDROM, DMA, SIO, dirty-RAM bitmap. **Audit each device snapshot for
timing/scheduling completeness** (scheduled event deadlines, in-flight DMA remaining
words + completion deadline, CDROM command phase + next-event cycle, GPU draw budget,
FIFO contents) — the save-state is taken at a *clean handoff*, so it may omit mid-
transfer transient state that a cycle-exact lockstep needs.

**Known CPU gaps (NOT in boot_state `CpuRegs`) — MUST add to the hash:**
- `muldiv_ts_done`, `gte_ts_done` (completion-stall deadlines)
- load-delay interlock: `read_absorb[33]`, `read_absorb_which`, `read_fudge`,
  `ld_which_t`, `ld_absorb`

**Known `interrupts.c` static gaps — MUST add:**
- `in_exception`, `post_exception_cooldown`, `dispatch_count`, `total_checks`,
  `cycles_since_vblank`, `g_irq_event_due_fast`, VBLANK schedule state, the async-RFE /
  precise-slice / dirty-interp resume state if it affects delivery.

**Exclude from the hash (host-only, would cause false divergences):** CPUState function
pointers, jmpbufs, fiber handles, malloc addresses, file handles,
struct padding, uninitialized bytes. Serialize little-endian explicitly; serialize
arrays in fixed order; serialize any event/deadline queue **sorted** by a deterministic
key (guest_cycle, priority, type, seq).

### Cheap per-block hashing

- RAM/scratchpad/VRAM: **incremental** hashing — 4 KB page hashes updated on write +
  a top hash; per-block read is O(pages touched), not O(2MB).
- CPU + devices: serialize the (small) canonical blob per block and hash it.
- `state_hash = H(cpu_hash, ram_hash, scratch_hash, vram_hash, device_hash)` — compare
  sub-hashes first on mismatch to localize which subsystem split.

## Validation gates — trust NOTHING until these pass

1. **compiled-vs-compiled = zero divergence across boot** (proves coordinator
   determinism + hashing + that we excluded all host-only state). Necessary, not
   sufficient.
2. **interp-vs-interp = zero divergence** (proves interp determinism + device
   instancing).
3. **hash-vs-byte audit:** every N segments, force a full byte compare even when hashes
   match (proves incremental-hash maintenance is correct).
4. **injected-divergence test:** a debug knob flips one RAM byte / one CPU reg in one
   process after seq K; the tool MUST halt at exactly that field/address. (Proves it
   actually catches divergence and names it correctly.)

Only after 1–4 pass do we run **compiled-vs-interp** and believe its first-divergence
report.

## Build plan

- [x] `COSIM_ORACLE.md` (this file).
- [x] State-hash module `runtime/src/cosim_state.c` (+ `include/cosim_state.h`):
      canonical FNV serialize → incremental RAM page hashes + CPU micro-state
      (muldiv/gte/load-delay) + interrupts statics + device blobs (boot_state accessors)
      → `state_hash` + sub-hashes. VRAM excluded in v1 (downstream, not causal).
- [x] Engine `runtime/src/cosim.c`: cycle-keyed checkpoints (ring + cumulative chain
      hash + park/step) + minimal standalone TCP server. Commands: `status`, `chain`,
      `stride N`, `runto <cycle>`, `hash`, `sub`, `window N`, `inject ram|reg`, `reset`.
- [x] Hooks (all `#ifdef PSX_COSIM`, zero effect on normal builds): `cosim_tick()` in
      `psx_advance_cycles` (the alignment clock); `cosim_block(pc)` emitted by BOTH
      emitters + called in the interp loop (pc reporting); `cosim_note_ram_write` in
      `memory.c` word/half/byte RAM writes (covers CPU+DMA — verified DMA routes through
      psx_write_*_raw); `interrupts_cosim_hash()`; `cosim_init()` in main.
- [ ] Clean build target `psx-cosim` (CMake): `PSX_COSIM` + `PSX_NO_DEBUG_TOOLS`,
      single-threaded guest, software renderer, no host throttle, headless. Add
      cosim.c + cosim_state.c to the target. (Fix any unguarded debug-only calls the
      `PSX_NO_DEBUG_TOOLS` build surfaces.)
- [ ] Regen game + BIOS (emitters now emit the guarded `cosim_block`).
- [ ] Coordinator `tools/cosim.py`: launch 2 instances (compiled + PSX_FORCE_INTERP),
      cycle-keyed lockstep via `runto`/`chain`, stride coarse→1 drill, halt at first
      divergent checkpoint, dump bounded `window` + `sub` diff.
- [ ] Run gates 1–4 (compiled-vs-compiled=0, interp-vs-interp=0, hash-vs-byte audit,
      injected-divergence halts at the right field), THEN compiled-vs-interp.

## History (do not lose again)

- The device-event IRQ ring (`device_trace`, commit `18c94a1`) + a prior session's
  Beetle diff found: native charges **zero guest cycles for GPU draw time** while
  Beetle halts the CPU (DrawTimeAvail/RecalcHalt), so native over-runs the data-paced
  asset-load loop (~5.4× GPU DMAs/frame) and completes `func_8001CB3C` ~150M cycles
  early → wrong branch. CD ruled out (faithful vs Beetle); MDEC decode cost
  implemented, did NOT fix. GPU draw cost bisect-confirmed as a **lever** but never
  validated to render the FMV. ChatGPT gave a full DrawTimeAvail implementation plan.
  **This is a hypothesis for the oracle to test, not an assumed answer.**
- This session (2026-06-30): localized to the load-delay axis on the current build
  (`PSX_LOAD_DELAY=0` → FMV plays, pixels), but that is a **secondary phase modulator**,
  not the faithful root; and a load-delay save/restore-across-exception fix was built
  and **refuted** (no effect). Two separate bugs confirmed: FMV-skip and the later
  `02 02` gameplay wedge.
