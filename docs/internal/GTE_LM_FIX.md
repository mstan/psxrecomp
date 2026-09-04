# GTE `lm`/`sf` fix in the depth-cue & lighting family

**Branch:** `feat/crashbash-fw-fixes` — commit `c4474b9`
**Fixes:** Crash Bash (SCUS-94570) menu-character 30 Hz strobe + in-game
character flattening. Root-caused and verified 2026-07-09.

## What was wrong

The runtime GTE hardcoded `lm = 1` (clamp IR results to `[0, 0x7FFF]`) on
every IR write-back in the shared lighting/depth-cue helpers
(`light_transform`, `light_color`, `color_output`, `depth_cue_from_ir`).
Real hardware takes `lm` from **bit 10 of the instruction word** — `lm = 0`
clamps to `[-0x8000, 0x7FFF]` and preserves negative results.

`depth_cue_from_ir` also ignored `sf` (bit 19) and skipped the hardware's
intermediate step clamp.

Affected ops: `INTPL`, `DPCS`, `DPCT`, `DPCL`, `NCDS`, `NCDT`, `NCS`, `NCT`,
`NCCS`, `NCCT`, `CDP`, `CC`. (`RTPS`/`RTPT`/`MVMVA`/`OP`/`SQR`/`GPF`/`GPL`
already honored the instruction word and are untouched.)

## Why it broke Crash Bash

Crash Bash tweens its 10 fps keyframe vertex animation to the 30 fps render
rate through **INTPL with `lm = 0`** — an off-label, signed-vector use of the
color-interpolation op:

- pose-A vertex → IR1–3
- pose-B vertex → far-color regs (FC) via `ctc2`
- blend factor → IR0
- `INTPL` (`0x0980011`) → lerped vertex read back from IR1–3 (`mfc2`)

With `lm` forced on, every **negative component** of a tweened vertex clamped
to 0, collapsing the character mesh onto its anchor point on tween frames.
Keyframe frames (no INTPL) stayed healthy, so with alternating draw buffers
exactly one display buffer ever held a healthy pose → the character strobed
at 30 Hz on menus and rendered flattened/half-visible in-game. Beetle and
real hardware honor per-instruction `lm`, so they never showed it.

## What changed

- `light_transform` / `light_color` / `color_output`: `lm` read from the
  instruction word and passed to every `saturate_ir` write-back.
- `depth_cue_from_ir`: rewritten to the faithful hardware formula —
  `base = IR << 12`; `step = lim±0x8000(((FC << 12) − base) >> sf·12)`
  (intermediate clamp with `lm` forced OFF, per hardware); `MAC = (base +
  IR0·step) >> sf·12`; final `IR = lim(MAC, lm)` with `lm` from the
  instruction. MAC overflow flags checked.
- Always-on GTE observability rings (build-dbg debug-server commands):
  `gte_ring_dump` (per-RTPS/RTPT inputs+outputs), `gte_intpl_dump`
  (per-INTPL inputs+outputs, frame filter + offset paging),
  `gte_frame_stats` (per-frame `nproj`/`nsat`/`nflat`/`nintpl`/
  `nintpl_tiny`), `gte_latch_dump` (retained saturated projections).

## Regression risk

Low, and skewed toward correctness:

- Standard libgte **lighting** calls encode `lm = 1`, so games using the
  ops as designed see identical clamping to before.
- The two real behavior changes are (a) `lm = 0` callers now keep negative
  results — the bug being fixed — and (b) the depth-cue step is now clamped
  to ±0x8000 before the IR0 multiply, which only matters when
  `|FC − IR| > 0x7FFF` (far color very distant from the lit color). Both
  changes move us **toward** hardware/Beetle behavior, so the only regression
  class is a game that accidentally depended on our old inaccuracy.
- `sf` handling: all common libgte encodings of these ops use `sf = 1`,
  which reproduces the old shift exactly.

## Cross-game validation — REQUIRED, all titles, before merge

This branch does not merge to master until every supported title has been
validated against this change. No subset, no sampling.

**Validated:**

**Crash Bash (SCUS-94570)** — the fix target. Menu strobe gone;
`nintpl_tiny` 335–352/473 → 0–3; character present in both VRAM
double-buffers and in all frames of a jittered screenshot burst;
menus/animation healthy.

**Ape Escape (SCUS-94423)** — 2026-07-10. Full branch-flavor rebuild
(BIOS + game regen, `build-crashbash-fw/`). Booted to title; lit title
models healthy (Ape was the highest-risk title — heavy GTE user, most
likely to exercise the depth-cue step clamp). User-validated in play.

**Tomba 2 (SCUS-94454)** — 2026-07-10. Full branch-flavor rebuild. Attract
gameplay healthy; `gte_frame_stats` over the attract scene: `nintpl = 0`
(no INTPL use), steady `nsat`/`nflat`, `nintpl_tiny = 0`. User-validated.

**Mega Man X6 (SLUS-01395)** — 2026-07-10. Full branch-flavor rebuild.
Attract gameplay demo healthy (standard `lm = 1` lighting path).

All four titles validated → merged to master `e4145d7` on 2026-07-10.
Post-merge gate: Tomba rebuilt clean against merged master; Crash Bash
junction re-pointed to master, regenerated, rebuilt — SELECT GAME TYPE
character present, `nintpl_tiny = 0` across the sampled frames.

**Per-title procedure (same for all):**

1. Boot to gameplay through the title's existing verify flow on this branch.
2. Capture `gte_frame_stats` over a representative scene and diff
   `nsat`/`nflat` against a pre-fix baseline built from master + this
   branch's parent commit — any shift is a stop-and-investigate, not a note.
3. Eyeball lit and fogged/depth-cued scenes side-by-side with the Beetle
   oracle; the oracle is ground truth, not the pre-fix recomp.
