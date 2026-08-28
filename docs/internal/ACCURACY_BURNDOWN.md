# PSX Accuracy Burndown (living doc)

Companion to FAITHFUL_TIMING_PLAN.md. That doc owns the CYCLE/TIMING axis (the
area under active implementation); THIS doc is the full-coverage burndown across
ALL accuracy axes for the faithful-core build.

## ⚠️ LESSON (2026-06-26): oracle-validate OUTPUT before applying a fix

The MDEC "fix" (agent branch `fix/mdec-faithful-accuracy`) was applied from
research-claimed source-level divergences and REGRESSED both Tomba 2 FMVs
(Whoopee Camp logo + Tomba intro) on color — REVERTED. The original MDEC output
was user-verified CORRECT. Takeaways, now binding:
- A research-claimed discrepancy is a HYPOTHESIS, not a bug. Validate the OUTPUT
  (diff our decoded result vs Beetle's on the SAME input) BEFORE changing code.
- "Matches Beetle's source conventions" does NOT justify rewriting code whose
  OUTPUT is already correct. Our impl may be a valid equivalent.
- The user's eyes / the oracle's OUTPUT override source-reading. "Looked right
  before, wrong after" = revert, always.
- MDEC fix is PARKED on its branch pending a real MDEC output-diff harness (decode
  the same stream on native + Beetle, compare blocks). Only fix proven output
  divergence. Do NOT re-apply blind.
- Same gate applies to ALL agent fixes (hybrid-pad etc.): reconcile onto wt/tomba2
  + validate vs the oracle BEFORE trusting.

Separately: the Tomba intro FMV left/right SPLIT seam is a PRE-EXISTING GPU/display
presentation bug (present before today's work), not MDEC — its own axis-5 item.

## Method (non-negotiable)

- Every item gets: **status**, the **external comparative(s)** to cross-reference
  it against, and a **validation method**. "Looks good" is NOT a status — an item
  is only GREEN once cross-referenced against a reference (psx-spx / Beetle source
  / DuckStation / a hardware test ROM) AND validated against the oracle at runtime.
  Self-agreement (compiled == our interp) proves backend-equivalence, NOT
  correctness — both can be identically wrong (CLAUDE.md §15).
- Don't do it all in one pass. Tomba 2 is the **stomping ground**: validate
  everything we can here, then validate the rest against the other games
  (Tomba 1, MMX6, Ape, BIOS), then merge wt/tomba2 → master, then keep a
  **post-merge burndown** for whatever remains.
- Governed by CLAUDE.md Rule -1 (faithful core, no hacks, breaking other titles OK).

## Comparative sources (the reference shelf)

- **nocash psx-spx** — the canonical hardware reference. Cite the section per item.
- **Beetle/Mednafen PSX** (IN-TREE: `psxrecomp/beetle-psx/mednafen/psx/`) — our
  oracle's own source: cpu.cpp, gte.cpp, gpu.cpp, spu.cpp, cdrom.cpp, dma.cpp,
  timer.cpp, frontio.cpp/`input/`, mdec.cpp, sio.cpp, spu/reverb. Read for the
  exact model; run as the oracle on port 4382 for runtime diff.
- **DuckStation** source — clean, heavily-commented; excellent cross-check for GTE
  cycle table, GPU rasterization rules, CD timing (use as a 2nd opinion vs Beetle).
- **Hardware test ROMs** (ground truth above emulators): Amidog CPU & GTE tests,
  PeterLemon/PSX, the psx-spx test suite, GPU/timing test ROMs. Running these on
  native vs Beetle is the strongest single validation we can add.
- **Real HW via Beetle oracle** (port 4382) — first-divergence on the relevant
  state surface (VRAM for GPU, audio samples for SPU, cycle counts for timing).

## Validation infrastructure to BUILD (prerequisite tooling)

- [ ] **Native↔Beetle cycle/first-divergence comparator** (replaces stale
  DuckStation-era find_divergence.py port 4371). Needs additive guest-cycle
  exposure in beetle_debug_server.c. The backbone measure for axes 2/3.
- [ ] **State-surface diffs**: VRAM byte diff (GPU), SPU sample-stream diff
  (audio), CD sector/response diff. Per-axis oracle comparators.
- [ ] **Hardware-test-ROM harness**: run Amidog/GTE/CPU test ROMs on native and
  Beetle, diff pass/fail + result registers. (Highest-leverage axis-1/2 validator.)

---

## Axis 1 — Instruction semantics (decoder)

Status: STRONG (recompiler core; proven byte-identical to interp on many funcs).
- [ ] ALU/shift/logical/sign-extension — cross-ref Amidog CPU test ROM.
- [ ] LWL/LWR/SWL/SWR unaligned — psx-spx "CPU Load/Store"; Amidog.
- [ ] MULT/MULTU/DIV/DIVU → HI/LO, div-by-zero & overflow results — psx-spx; Amidog.
- [ ] Overflow-trapping ADD/ADDI/SUB (vs ADDU/etc.) — do we trap? psx-spx "Exceptions".
- [ ] **Load-delay slot hazard** — KNOWN SIMPLIFICATION: interp loads land
  immediately (no 1-instruction delay). Cross-ref psx-spx "CPU pipeline"; Beetle
  LDAbsorb. Decide: model it or document why safe.
- [ ] Branch-delay slot (incl. branch-likely absence on R3000) — done; spot-check.
- [ ] COP0 (mtc0/mfc0/rfe, Status/Cause/EPC/BadVaddr) — psx-spx "COP0".
- [ ] GTE/COP2 math: fixed-point, saturation, FLAG register, all 30+ ops —
  cross-ref Amidog **GTE test ROM** (the definitive validator) + DuckStation gte.cpp.

## Axis 2 — Cycle/timing  ← ACTIVE (see FAITHFUL_TIMING_PLAN.md)

Status: Stage-1 (1 cycle/insn, single-source seam in place); Stage-2 in progress.
Oracle model fully transcribed in `CYCLE_MODEL_BEETLE.md`. Game-independent
BIOS-kernel ruler BUILT (region [0x80001C5C→0x80001CA4]); per-block-leader cycle
observe added to the recompiler so ANY block leader is anchorable on both backends.
- [x] Single-source `psx_instr_base_cycles` seam (identity), both backends.
- [x] cyc_watch dispatcher+prologue double-fire dedupe (was corrupting native Δ).
- [x] Per-block-leader observe (native now samples like Beetle, not only fn-entry).
- [x] **Load double-count FIXED**: Stage-2 #1a put +2 data-access in
  psx_instr_base_cycles while memory.c already charged +6/main-RAM-load → counted
  twice. Reverted opcode fn to pure execute base; memory.c is the single address-
  keyed owner. Ruler [c5c→ca4]: native 34→30 (exact), no FMV regression.
- [x] **Memory wait-state — DONE (full R3000A load model).** load=4 LANDED (the boot
  wedge was a real device-timing bug the accurate load exposed — pad ACK→IRQ7 made
  guest-cycle-paced, see axis 5; commit d8c4a8e). ReadFudge + LDAbsorb give-back state
  machine SHIPPED across both emitters + interp (psx_cyc.h §1/deps/DO_LDS; commits
  d8c4a8e/fade560/d597797) — load2 10→11. **Device-region MMIO read waits DONE** (commit
  9ae534d, branch wt/tomba2-mmio-waits): Beetle MemRW table in psx_cyc_readmem, size-aware
  (SPU +36/+16, CDC +6×size, GPU/MDEC/SysCtrl/FIO/SIO/IRQ/DMA/Timers +1, RAM +3, else +0).
  Ruler #2: all 15 loops (incl. mmio_timer +3, mmio_spu +38) == Beetle EXACT.
  RESIDUAL: DMACycleSteal (dynamic per-read DMA bus-steal, libretro.cpp:868) unmodeled.
- [x] **Mult/div completion-stall — DONE, validated EXACT in BOTH emitters**
  (commits a3e8f28 game, 180b821 BIOS). CPUState.muldiv_ts_done set by
  MULT/MULTU/DIV/DIVU, MFLO/MFHI stall to it (psx_cycles.c). Per-instruction
  charging default on this branch (both emitters). Ruler #2 (game): div +38,
  div_spaced +38 (absorb), mult +15 — ALL == Beetle. Ruler #1 (BIOS kernel):
  [c5c→ca4] native 30→56 == Beetle 56, STEADY DELTA 0 EXACT. FMV no regression.
- [x] **Instruction-fetch / I-cache — DONE both backends.** Stage 1 (interp, commit
  958a928) + Stage 2 (BOTH static emitters at cache-line leaders via the runtime PC;
  relocate_ra for BIOS shell/kernel; default-on; commit 0edb935). Faithful direct-mapped
  4 KB/256-line model (psx_icache.c, from Beetle ReadInstruction: +0 hit / +4 KSEG1 /
  +3+refill miss). Ruler #2 icache_miss == Beetle; ruler #1 [c5c→ca4] steady 56==Beetle
  AND native now produces the cold-refill spikes (77/84) that were absent. FMV no-reg.
- [x] **GTE per-command cycles — DONE, validated EXACT** (commit ec1fd76).
  CPUState.gte_ts_done armed in gte_execute (cost-1 table verified from beetle
  gte.cpp op returns; AVSZ4=5 not 6); any COP2 reg access stalls to it; both
  emitters + interp. Ruler #2 gte_rtps +15 / gte_nclip +8 == Beetle. FMV no-reg.
- [x] **Mult/div stall — also completed to the dirty-RAM interpreter** (commit
  75d5d1a): backend parity (interp was charging 0 for mult/div). By-construction
  + no-regression (no interp-path ruler yet — see below).
- [x] Instruction-fetch / I-cache timing — DONE (see above; commits 958a928 + 0edb935).
  The ruler's 56→84 cold spread (I-cache line-refill transient) now reproduced natively.
- [ ] **HW test-ROM ruler (#2)** — Amidog CPU/GTE timing ROMs for hand-crafted
  single-COMPONENT isolation (div-only, load-only loops) that organic BIOS code
  can't give (the prologue combines div+loads in one block). Strongest validator.
- [ ] Validation: native cumulative cycles == Beetle == analytic on the rulers.

## Axis 3 — Interrupt / event timing

Status: PARTIAL.
- [ ] Device IRQ-raise timing (VBLANK scanline, timer overflow/target, DMA/CD
  completion) — tied to axis 2/5; psx-spx per device.
- [ ] **IRQ take-point** (HW = exact instruction; us = block edge) — the parked
  precise-slicing (PRECISE_IRQ_SLICE.md). Validate vs Beetle exc_ring.
- [ ] Exception entry record (EPC/Cause.ExcCode/BD/Status-stack) — currently uses
  a sentinel EPC; cross-ref psx-spx "Exceptions"; Beetle. Validate exc_ring match.

## Axis 4 — Memory map / MMIO

Status: MODERATE-STRONG (regions games use).
- [ ] KUSEG/KSEG0/KSEG1 mirroring, scratchpad, cache-isolation (IsC) — psx-spx.
- [ ] I/O register semantics: read-to-clear, write-1-ack (I_STAT), masking,
  unmapped/garbage reads — psx-spx "I/O Map"; Beetle memory.cpp.

## Axis 5 — Peripherals / devices  ← SUSPECTED WEAKEST (user flag)

Status: MIXED — "works for tested games," NOT edge-validated. Likely more gaps
than we think; the **hybrid-pad failure in Tomba is an axis-5 (SIO/controller)
bug**, not timing.
- [~] **SIO / controllers / memcard**: DualShock config-mode handshake (0x43),
  analog vs digital pad ID, the **hybrid pad mode failure** (Tomba) — Beetle
  frontio/input + psx-spx "Controllers and Memory Cards". HIGH PRIORITY per user.
  - DONE 2026-06-27: **pad ACK→IRQ7 timing made guest-cycle-paced** (was
    access-count-paced via `sio_irq_countdown=SIO_IRQ_DELAY_PAD`; pad fast-path
    now arms the cycle-paced ack scheduler, BAUD+ACK=1258 cyc, like the card
    path). This was the cause of the load=4 Tomba 2 boot wedge (accurate CPU
    exposed access-paced pad timing). See WEDGE_load4_shell_rootcause.md.
  - DONE 2026-07-26: Tomba now offers explicit Analog / D-Pad modes; the
    Tomba-only legacy pad-config fork and its debug/config surface were removed.
    Every title now uses the modern DualShock config state machine.
  - TODO (axis5 Fix-6 / "1.0e-e2"): fully remove the pad fast-path so pad+card
    share the unified shifter path.
- [ ] **GPU**: GP0/GP1 command set, rasterization rules (top-left fill, dithering,
  semi-transparency modes, mask bit, texture windows, blending), VRAM-as-texture —
  cross-ref DuckStation gpu_*, Beetle gpu.cpp, GPU test ROMs; validate by VRAM diff.
- [ ] **SPU**: 24 voices, ADSR, pitch/sample-rate, reverb, volume sweeps, IRQ —
  Beetle spu.cpp; psx-spx "SPU"; validate by audio-sample diff.
- [ ] **CDROM**: command set, response timing, sector read (data/XA/CD-DA), seek,
  shell/lid — Beetle cdrom.cpp; psx-spx "CDROM"; validate by sector/response diff.
  - shell/lid is entirely absent today (`CDSTAT_SHELL` is a pure function of
    `has_disc()`, no open transition / no latch / no INT5-while-open). It is
    planned as P3 of `../MULTI_DISC.md`; update this item there, not in a
    parallel record.
  - **KNOWN OK-TO-DIVERGE (we are MORE faithful than the oracle) — from PR #9:**
    Suppressed in `tools/devtrace_diff.py` (`KNOWN_DIVERGENCES["cdrom"]`; use
    `--strict` to see raw). Beetle stays the independent oracle (unpatched); the
    tool LABELS these deltas, it does not silence real ones.
    - **CD controller version (Test `19h`,`20h`)**: we return `94 09 19 C0` (real
      SCPH-1001 sub-CPU, 1994-09-19, per psx-spx "CDROM Test Commands"). Beetle
      hardcodes `97 01 10 C2` (`cdc.cpp:2253`, a later PSone board) regardless of
      BIOS. Version byte `< 95h` keeps shell CD-init flag `[0xA000DFFC]` clear;
      `>= 95h` sets it and forces a spurious boot ReadTOC. So native (94h) issues
      NO ReadTOC where Beetle (97h) does → expect a CDROM event delta at CD-init,
      possibly cascading briefly. Verified vs psx-spx, NOT the oracle. MMX6+Tomba
      soak PASSED (2026-07-06). Trivially revertable (4 bytes, `cdrom.c` Test 0x20).
    - **Status reg `0x1F801800` bit 2 (ADPBUSY)**: idles at 0 (was wrongly pinned
      set under an "ADPCM empty" label). psx-spx: bit2 = XA playback → 0 when idle.
    - **GetID license region**: derived from disc serial (`SCEA`/`SCEE`/`SCEI`)
      instead of hardcoded `SCEI`; NTSC-U (MMX6/Tomba) now correctly report `SCEA`.
    - Method reminder (top-of-doc LESSON): these were HYPOTHESES until OUTPUT
      validation — the MMX6/Tomba playtest soak WAS that validation. Revert any
      that regress a shipped title.
- [ ] **DMA**: all 7 channels, block/linked-list/chain modes, timing, DICR/DPCR —
  Beetle dma.cpp; psx-spx "DMA".
- [ ] **MDEC**: macroblock decode, IDCT, color conversion, RLE — Beetle mdec.cpp;
  validate FMV frame diff vs Beetle.
- [ ] **Timers (0/1/2)**: all clock sources (sysclk/dotclock/hblank/÷8), modes
  (target/overflow/reset/IRQ-repeat/one-shot), sync modes — Beetle timer.cpp;
  psx-spx "Timers".

## Axis 6 — Static-vs-dynamic fidelity (recompiler-unique)

Status: STRONG (most project effort lives here).
- [ ] Self-modifying / install-at-runtime code (dirty-RAM interp) — ongoing.
- [ ] Function discovery / dispatch completeness (no missed indirect/jump-table
  targets) — resolve all dispatch misses each run (Tomba2Recomp CLAUDE.md).
- [ ] Call/return contract + stack fidelity — the blue-screen/wedge class.
- [ ] Backend equivalence (compiled == interp) — necessary, not sufficient.

## Axis 7 — Determinism

Status: SOFT SPOT.
- [ ] Boot run-to-run variance observed (sometimes wedges, sometimes clean) —
  track down; faithfulness presupposes determinism.

---

## Research findings (2026-06-26, parallel subagents, cross-referenced vs in-tree Beetle + psx-spx)

Per-axis deep-dives in `accuracy/*.md`. Headlines + priorities below. NOTE much is
already faithful — these are the GAPS. Implement serially (one branch each, code to
just-before-build, pull in + validate on the oracle one at a time). NONE of these
overlap the cycle axis or each other (different files), so they parallelize.

- **`accuracy/axis5_sio_controller.md` — HYBRID PAD BUG ROOT-CAUSED.** P0: cmd `0x43`
  must transmit the LIVE button frame (like `0x42`) + use the trailing byte to toggle
  config; we send all-zeros → active-low → "all buttons pressed" phantom input every
  `0x43` frame. P0: `0x45` status must report the LIVE analog/digital mode, not always
  analog-ON (→ frame-length misparse after a flip). Both were fixed, and the
  Tomba legacy fork was deleted 2026-07-26 when Tomba moved to explicit
  Analog / D-Pad modes. P1: analog-mode lock (`0x44`); P2: `0x4D`
  rumble echo + cycle-paced pad ACK. Validate: `sio_trace` diff on `0x43`/`0x45`.
- **`accuracy/axis5_gpu.md`.** P1: dithering ENTIRELY MISSING in both renderers (decoded
  but never forwarded) — largest systematic banding divergence. P2: SW rasterizer uses
  float + inclusive spans vs HW fixed-point half-open DDA w/ top-left bias → shared-edge
  double-draw + 1px over-fill. P3: sprite FlipX/FlipY unmodeled; mono-rect size mask
  wrong. FAITHFUL: all 4 semi-transparency modes, mask set/check, texture-window, CLUT
  4/8/15, texel-0, ×2 modulation, GP0/GP1 coverage. Validate: VRAM byte-diff + GPU test ROMs.
- **`accuracy/axis5_spu.md` — weakest subsystem.** P0: reverb ENTIRELY MISSING; P0:
  volume sweeps missing + ~2× gain-scale error; P1: noise gen + pitch-modulation absent;
  P1/P2: no SPU IRQ address-match, no capture buffers, SPUSTAT hardcoded; P1: host-audio-
  pull timing not guest-768-cycle clock. FAITHFUL: ADPCM decode + ADSR envelope (verbatim
  Beetle ports). Recommend: port Beetle PS_SPU on a guest-clock timeline. Validate: audio
  sample-stream diff.
- **`accuracy/axis5_mdec.md` — explains the slightly-off FMV.** HIGH: IDCT rounding/clamp
  diverges (contrast/ringing); HIGH: YUV→RGB lacks HW green-precision truncation (off-hue
  greens); HIGH: dequant domain/bias/clamp wrong (DC banding). MED: 4bpp output broken
  (textures, not FMV); MED: one-shot decode vs FIFO/block state machine. Validate:
  per-block decode diff + live FMV pixel-diff.
- **`accuracy/axis4_memory_mmio.md`.** GOOD: I_STAT/I_MASK/DICR ack semantics CORRECT.
  P1: RAM mirror wrong (we gate `<2MB`; HW aliases 2MB DRAM across an 8MB window 4× —
  mirror accesses silently read 0 / drop writes). P2: `IsC` over-broadly drops scratchpad
  writes (scratchpad is the D-cache, must stay addressable). P3: level-IRQ relatch; P4:
  per-segment addr masking; P5: open-bus high bits on I_STAT readback. Validate: MMIO
  trace diff + RAM-mirror sentinel probe.

Cross-cutting: every validation is a native(4500)↔Beetle(4382) ring-buffer diff on the
relevant state surface — the same oracle methodology as the cycle axis. Several need the
test-ROM harness (axis 1/GPU) which is worth building early.

## Phasing

1. NOW: cycle axis (axis 2) Stage-2 on Tomba 2 (FAITHFUL_TIMING_PLAN.md).
2. Build the comparator/test-ROM tooling (enables GREEN-ing items above).
3. Burn down axes here on Tomba 2 where validatable; axis 5 (esp. SIO/controller
   for the hybrid-pad bug) is the priority second front.
4. Validate cross-title (Tomba 1, MMX6, Ape, BIOS) before merge.
5. Merge wt/tomba2 → master; keep this doc as the post-merge burndown for the rest.
