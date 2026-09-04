# Generic backdrop preload (widescreen) — design + implementation plan

> **2026-06-16: ROOT CAUSE CORRECTED — read the "2026-06-16" section at the bottom
> FIRST.** The column-preload design below was the wrong lever for the 16:9 void;
> the real cause is native-wide 2D-backdrop POSITIONING and the fix is a GL
> wide-mirror geometric stretch gated to the backdrop (mechanism proven; precise
> classification via `gp0_cmd_source_addr` / sprite-tag is the remaining step).

Branch: `feat/ws-backdrop-coverage` (both repos, off master). Goal: in 16:9,
Tomba's far-BACKGROUND tile grid (sky/ground/cloud/flower-field layers) only
generates a camera-centered ~4:3 window of tile columns, so the revealed right
(and a little of the left) margin shows void until the camera moves. User
directive: **eliminate the culling / preload the entire background** (free on
modern HW; the extent is a finite tile table), as a **generic auto-detect** that
works across all scene overlays and is reusable for any PSX game.

## Root cause (fully diagnosed 2026-06-15)

The backdrop generator is **overlay-resident AND scene-specific**: each area's
overlay carries its own copy of a structurally-identical column-window generator
at its own addresses (the village's is `FUN_80116a28`; the sky/flower-field
scene's is `~0x80116800`; the bytes at a given address differ per scene). The
main EXE has NO camX cull — it tail-dispatches to these overlay handlers
(verified by byte-searching every `0x176` consumer in SCUS_942.36).

Per window, the generator computes a camera-windowed column range:
```
camX   = lh/lhu rC, 0x176(rB)              ; camera world-X
(opt)    addiu rC, rC, Koff                ; per-layer parallax bias (varies)
start  = (camX+Koff) / 96                  ; signed reciprocal-multiply:
           lui rM,0x6666 ; ori rM,rM,0x6667 ;  (magic 0x66666667)
           mult rC,rM ; mfhi rH ; sra rQ,rH,5 ; subu rQ,rQ,(rC>>31)
         move rStart, rQ                   ; START finalize
end    = addiu rEnd, rStart, N             ; N = 4:3 window width (8..12; some 44/70)
         <start clamp low>                 ; bgtz->li 1, or bgez->clear 0
         <end clamp high>                  ; slt rEnd,rExt -> addiu rEnd,rExt,-1
                                           ;   (rExt = lbu tile-table length, finite)
                                           ; or slti rEnd,Cext -> li rEnd,Cext-1 (const, finite)
inner loop: for col in [start, end): emit one tile from the finite table
```
The window slides with camX → the 16:9-revealed columns aren't generated. Fix =
force `start=0` (loop from col 0) and `end=full extent` (the existing high clamp
pins it), so the whole finite row is emitted every frame regardless of camera.

## The invariant signature (verified on BOTH the village + flower-field overlays)

Byte/opcode-STABLE anchors (match on these):
- **`0x176` camX half-word load** (`lh`/`lhu rC,0x176(rB)`) — offset invariant.
- **Magic `0x66666667`** — `lui rM,0x6666` + `ori rM,rM,0x6667`. Strongest anchor
  (the /96 reciprocal is specific to these scrollers; gating on the `0x176`
  camera offset essentially eliminates collisions).
- **Divide tail** `mult rC,rM` → `mfhi rH` → `sra rQ,rH,0x5` → `subu rQ,rQ,rSign`
  (shift amount 5 invariant).
- **`addiu rEnd,rStart,N`** end-delta with small N, immediately after the
  `move rStart, <quotient>` start-finalize.
- **End clamp** to the tile-table length register (`slt …,rExt`→`addiu rEnd,rExt,-1`)
  or a literal (`slti …,Cext`→`li rEnd,Cext-1`).

VARIES (read from the matched site, never assume): all registers
(village a2=start/a3=end/t0=mfhi; FF s2=start/s1=end/a2=mfhi), the parallax
offset Koff (and whether a `jal 0x8002d964` cam-jump precedes), N (8/9/10/12 and
44/70), the start-clamp floor idiom (floor 0 vs 1 vs const), the extent source
(register `s2`/`s7` vs literal `0x20`/`0x3f`), and the number of windows per
jumptable case (village 1; FF 1–3, two-layer backdrops).

## Patch (two single-instruction rewrites per window; both confirmed clean)

1. **START finalize** `move rStart, rQ` → force `rStart = 0` when widescreen
   active. The existing low clamp keeps it ≥ floor.
2. **END delta** `addiu rEnd, rStart, N` → force `N = 0x100` (≥ any 8-bit table
   length) when widescreen active. The existing high clamp pins `rEnd` to
   `extent-1`. (For const-extent windows, ≥ the literal ceiling, e.g. 0x3f.)

`extra=0` / widescreen-off must reproduce the original N and the original
`move` exactly (byte-identical faithful build).

Full extent is a finite `lbu` tile-table length (≤255, realistically dozens) or
a small literal → preloading the whole row is bounded and safe (no OOB; the emit
loop writes one display-list slot per column from the finite table).

## Implementation plan

### Recompiler pass (gcc/native path) — `[widescreen.cull] auto_backdrop = true`
A function-level pre-pass in `code_generator.cpp` (sibling to
`func_has_screen_extent_cull`): for each function, find every window via the
invariant (magic 0x66666667 + a `0x176` load feeding the `mult` + the
`sra…,5`/`subu` divide tail + `move rStart,quotient` + `addiu rEnd,rStart,N`).
Record a map `{addr -> {START_ZERO(reg) | END_WIDEN(rs,reg)}}`. `translate_instruction`
consults it: at START_ZERO emit `rStart = psx_ws_backdrop_preload() ? 0 : <orig move>`;
at END_WIDEN emit `rEnd = rStart + (psx_ws_backdrop_preload() ? 0x100 : N)`.
`psx_ws_backdrop_preload()` = a runtime predicate (gpu.c), nonzero only when
native-wide is engaged (0 at 4:3 ⇒ byte-identical). Robustness: the function-level
magic+0x176 gate + the divide-quotient dataflow gate make false positives very
unlikely; assert/log the per-function window count at build time.

### Overlay coverage (the generators are overlay-resident)
- The recompiler pass must run for the OVERLAY compile too: `compile_overlays.py`
  already forwards `--ws-config` (game.toml), so `auto_backdrop` reaches it. Bump
  `PSX_OVERLAY_CODEGEN_VER` and rebuild the overlay cache (gcc/<arch-abi>/cgN).
- Backends: a backdrop generator may run as a gcc-cache DLL (pass handles it),
  a sljit shard, or interpreted. [sljit was removed 2026-07-15 (`5b7e69b4`); the
  toolchain-free tier is now bundled tcc, which also produces a DLL — so only the
  DLL and interpreted cases exist today. See `runtime/src/overlay_backend.c`.]
  For full coverage like `auto_screen_x`, the
  sljit emitter + dirty_ram_interp need the same force-start-0 / widen-end at the
  detected sites. Options: (a) export the detected site map to the runtime so
  sljit/interp can apply it, or (b) re-derive the signature in a sljit/interp
  pre-scan (heavier). NOTE: in dev, overlays often run interpreted — so the
  interp hook is needed to SEE the fix in the debug build.

### Verify
Debug build (build-stable, TCP) navigated to the flower-field/sky-platform scene
(user drives). Confirm the right-margin void fills (full row preloaded) and 4:3
stays byte-identical. Extracted overlay for offline analysis:
`_shots/overlay_flowerfield.bin` (base 0x800E7000); also imported into Ghidra as
`overlay_flowerfield.bin`. Village reference: `overlay_800E7000.bin` /
`_shots/overlay_800E7000.bin`.

## Status
IMPLEMENTED 2026-06-15 on `feat/ws-backdrop-coverage` (all 4 execution paths);
LIVE-TESTED then PIVOTED — see "Pivot" below; pending re-verify + merge. Related:
`../WIDESCREEN.md`, `NATIVE_WIDE_PLAN.md`, memory `ws_draw_census_8c` /
`native_wide_fov_autocull`.

### Pivot (2026-06-15, after first live test)
First approach forced START→0 / END→0x100 (draw the WHOLE finite row). Live test:
village backdrop void FIXED but **laggy** (whole-row × 4 layers = massive
overdraw, amplified because the generator runs interpreted in the dev build), and
the watchtower scene still **culled on the right** (its row > the fixed 0x100
cap). Switched to **camera-tracked window WIDENING**: keep the camera-relative
window, extend START left / END right by a margin in COLUMNS sized to the 16:9
reveal. The detector now also records `window_cols` (= |addiu offset|, the
window width); `psx_ws_backdrop_value(orig, is_end, window_cols)` shifts the bound
by `margin = window_cols * nw_offset / disp_w + 2`. Elegant property: per-side
coverage = margin_cols × col_width = (N·nw_offset/disp_w)·(disp_w/N) = nw_offset
px = EXACTLY the reveal, for every layer regardless of window_cols (+2 col slack
for truncation/pop-in). Draws only the now-visible columns → no overdraw (fixes
lag), no fixed cap (fixes the right-gap). Codegen ver 2→**3**, ABI value callback
gains the window_cols arg.

Also fixed a pre-existing blocker the boot smoke-test exposed: the explicit
`[widescreen.backdrop] x_sites` / `[widescreen.cull] *_sites` shape checks
hard-`exit(1)` on a non-matching opcode, but overlay regions reuse the same
address for different code across scene VARIANTS (x_site 0x8012196C is `sh` in one
variant, `addiu` in another). Added `CodeGenConfig::overlay_mode`: in overlay mode
a shape mismatch SKIPS the transform (applies it only where bytes match) instead
of killing the whole overlay-variant compile; main-EXE mode keeps the hard error.

### What shipped (uncommitted on feat/ws-backdrop-coverage)
- **Shared detector** `runtime/include/ws_backdrop_detect.h` — self-contained C
  (stdint only), `psx_ws_find_backdrop_windows()`. ONE source of truth for all
  paths (recompiler includes it via relative path — runtime/recompiler both have
  a `gte.h`, so no include-dir merge). Gate per `mult`: bounded BACKWARD scan for
  the exact magic (lui 0x6666 + ori 0x6667) into one operand + a `lh/lhu …,0x176`
  into the other (the magic is sometimes set in a shared branch-delay slot, so a
  forward-accumulating scan corrupts on the branch — backward-per-mult is the fix);
  forward divide tail (mfhi→sra,5) + the move/addiu consumers. **Role by OFFSET
  ORDERING** (smaller offset → START→0, larger → END→0x100), NOT move-vs-addiu:
  village + flower-field each have a window where the `addiu` (negative offset)
  is START and the `move` is END. The plan's "move=START/addiu=END" was WRONG.
- **Runtime** `gpu.c`: `psx_ws_backdrop_preload()` (= `ws_native_wide_active()`,
  0 at 4:3/squash/FMV) + `psx_ws_backdrop_value(orig,is_end)` (identity unless
  preload → 0/0x100). One helper, all backends, like `psx_ws_cull_sltiu`.
- **gcc/native**: `[widescreen.cull] auto_backdrop` (config_loader + main_psx +
  code_generator `detect_backdrop_windows()` + translate_instruction emit + extern).
- **interp** `dirty_ram_interp.c`: `ws_backdrop_site_kind(pc)` windowed+cached
  scan, applied at exec_one top (gated on preload). NEEDED for the dev build
  (overlays run interpreted).
- **sljit** `overlay_sljit.c`: PASS-1 fragment detection → `s_bd_kind[]` →
  emit_one routes the move/addiu through psx_ws_backdrop_value.
- **overlay ABI**: `ws_backdrop_value` callback appended (overlay_api.h),
  PSX_OVERLAY_ABI_VERSION 3→**4**, PSX_OVERLAY_CODEGEN_VER 1→**2**; wired in
  overlay_loader.c + the compile_overlays.py DLL glue.
- **game.toml**: `auto_backdrop = true`.

### Verified
- Detector vs ground truth: flower-field 12 sites (incl the swapped-role window
  DE0=END/DE8=START), village 8 sites (a2/a3 alloc, floor 1) — 0 false positives
  over ~91k words. Main-EXE regen: 0 windows (generators are overlay-resident).
- gcc emit: synthetic recompile of the flower-field overlay (seed 0x80116800)
  emits exactly 12 `psx_ws_backdrop_value()` calls at db0/db8/de0/de8/e24/e2c/
  168e8/168f0/169a0/169a4/e64/e68, correct START/END roles, byte-identical at 4:3.
- recompiler + runtime + overlay-cache (cg2) all build clean; runtime boots (TCP
  4470, ws mode=2).

### Remaining
- LIVE-SCENE verify (user navigates the flower-field/sky-platform scene in 16:9;
  confirm right-margin void fills + 4:3 byte-identical; `wide_shot` to capture).
  In the dev build the generator runs INTERPRETED → the interp hook applies it;
  overlay_cache=true also self-heals a cg2 DLL with the rewrite once the scene is
  captured + autocompiled.
- Merge `feat/ws-backdrop-coverage` → master (both repos), bump Tomba pin, push.
- sljit coverage is fragment-local (a window whose magic load falls in a different
  fragment than its mult isn't detected on the sljit path — interp/gcc cover it).

---

# 2026-06-16 — ROOT CAUSE CORRECTED. The column-preload above was the WRONG LEVER.

Everything above (auto_backdrop column preload: force START→0 / END→extent to draw
the whole finite tile row) is **superseded as the fix for the 16:9 void.** It is
not wrong code, but it does not address the symptom. Live RE on the dwarf/cursed
flower-field scene, debug-instrumented, debunked the premise.

## DEBUNKED: the void is NOT a column-loading problem
Always-on `ws_backdrop_ring` on the live scene: the active flower-field generator
fires exactly ONE window (wcols=9, extent=41). Forcing whole-row (START→0,
END→clamped 40 — the entire 41-col row loaded every frame) and the cyan void
**still persists**. Loading more columns is irrelevant. The `ws_backdrop_margin`
knob (live column-widen: <0 whole-row, 0 off, >0 N-cols) confirms this: off vs
whole-row are pixel-identical on the void.

## ACTUAL ROOT CAUSE: native-wide 2D-backdrop POSITIONING (3 confirmations)
1. The flower-field is a **fixed-screen-position 2D tile field**: static GPU
   primitive vertices (signed-11-bit SCREEN coords — read the same at cam 1585 AND
   1550, they do NOT move with the camera), ~4:3 width (X extent ≈ -92..252 ≈
   344px). The "scroll" swaps which columns fill the fixed slots (the camera
   window), it does not move the slots.
2. Native-wide widens **3D via the GTE** (the un-squash generates wider geometry at
   the source). The 2D backdrop **bypasses the GTE**, so it stays 4:3-width and
   cannot span the 426px native-wide frame → the layer behind (cyan) shows on the
   trailing margin. `psx_ws_backdrop_x` (the existing 2D-backdrop screen-X widen) is
   SQUASH-only (`ws_active()`); in native-wide `gpu_state` shows `configured=0,
   active=0` → it is a no-op.
3. The GL native-wide compositor mirrors each prim into the wider FBO by
   **translation only** (`wide_dx() = g_wide_off - base_x`, gpu_gl_renderer.c). 3D
   fills the FBO because it is pre-widened; the 2D backdrop is centred → margins.

## THE FIX — mechanism PROVEN, classification is the remaining work
**Scale the 2D-backdrop prims about screen-centre in the GL wide-mirror**: a
geometric scale in the vertex shader `x' = (x - u_xcenter)*u_xscale + u_xcenter`,
scale = g_wide_w/native_w ≈ 1.33. This stretches the layer — tiles widen AND
spread together, no inter-tile gaps. **PROVEN (user-confirmed): with the gate
disabled, the backdrop fully fills the 16:9 frame, void gone.** Shipped runtime-
tunable: `ws_backdrop_stretch [on] [pct] [thresh] [mode]` (live).
Implementation (gpu_gl_renderer.c, this branch, uncommitted→committed checkpoint):
u_xscale/u_xcenter uniforms in the geo+tex programs (default 1.0/0 = bit-identical
no-op) + `wide_set_bd_scale` in both wide-mirror passes; per-frame phase reset in
`gl_perf_present_enter` (NOT `present_vram` — that is the 4:3-fallback present; the
native-wide present is `gl_renderer_present_wide_fbo`, and the per-frame hook must
be in the common `gl_perf_present_enter` both call).

### The remaining problem: stretch ONLY the background
Stretching everything also stretches Tomba + HUD + 3D (user-rejected). Draw-order
"phase" heuristics FAILED because:
- the far-parallax backdrop (ocean/cloud/mountain, FUN_8004db3c, GTE-projected) is
  WIDE and draws FIRST — and is ALREADY correctly widened by the 8C un-squash, so
  it must NOT be re-stretched; the flower-field (2D, narrow) draws AFTER it; so
  "first wide prim ends the phase" clears on the far-parallax before the flowers.
- Tomba/HUD draw last but are WITHIN 4:3 (narrow), so "narrow" includes them.

### BREAKTHROUGH — draw-time classification IS possible (the path forward)
The draw path HAS the per-prim **source address via `gp0_cmd_source_addr`** (gpu.c)
— that is how `ws_tagged_anchor` does the HUD pivot at draw time. So:
- Tomba/HUD/characters are **sprite-tagged** (`psx_ws_sprite_tag` stores
  prim_ptr→anchor in `ws_tags`, keyed by `$a0 & 0x1FFFFC`). At draw time
  `ws_tagged_anchor(gp0_cmd_source_addr)` matches them → foreground → DON'T stretch.
- The flower-field tiles live in the backdrop data structure `a1` (e.g.
  0x801a603c: extent byte @+0, table of packet offsets @+4, packets at
  a1+table[col]); their `gp0_cmd_source_addr` falls inside that structure's range
  (capturable from the generator's a1 via the ring).
- **PROPOSED GATE** for `wide_set_bd_scale`: stretch a wide-mirror prim iff
  native-wide AND NOT sprite-tagged (invert `ws_tagged_anchor` on
  `gp0_cmd_source_addr`) AND narrow/not-GTE-wide. (Or match `gp0_cmd_source_addr`
  against the known backdrop-structure address range.) This is the next concrete
  step — wire `gp0_cmd_source_addr` into the GL wide-mirror gate. NOTE: this needs
  the GL path to receive `gp0_cmd_source_addr` per prim (it is gpu.c state today).

## GENERATOR (RE'd via Ghidra `overlay_flowerfield.bin`)
0x80116808: extent s7=[a1+0] (byte); dispatch by backdrop type (jumptable
0x8011523c, 5 types); each type computes a column window then `j` to the SHARED
emit loop 0x80116e6c, which clamps START<0→0 / END≥extent→extent-1 and stores
packet pointers (a1+table[col]) into the OT object (a0, count byte @a0+3). One
window per call. No code xref / no caller in RAM (main-EXE tail-dispatch; not a
function pointer in the EXE/overlay binaries either).

## FREEZE in this area — INDEPENDENT of widescreen (dedicated read-only RE pass)
The recurring fatal wedge is the pre-existing Tomba **wild-call / runaway-recursion
family** (seesaw / Bug-D), NOT a kernel livelock (the 0x2104 thread thrash + epc
0x80000048 are the HEALTHY VSync scheduler; I was wrong to suspect a livelock).
Chain: seesaw entity loop func_8001DFD4 → interaction-queue dispatcher FUN_80054D60
→ a WILD jal in overlay handler 0x80123EE0. The Bug-D contract contains it but
never resolves (bail_resolved=0, ~93K flattens) → infinite per-frame storm →
eventually exhausts a fiber stack (report "native stack guard — runaway recursion",
recursion_func 0x8004DEE0) OR func_80056E08 derefs a garbage interaction-queue slot
→ MMIO READ8 @ 0x1F803C00 (the LOAD-MENU crash). Root fix = Beetle-oracle
data-divergence hunt for the corruptor; containment = skip the dead entity in the
flatten path (full_function_emitter.cpp). See TombaRecomp/ISSUES.md.

## DEBUG TOOLING SHIPPED (runtime-only, this branch)
- `overlay_cache=false` (all overlays interpreted; no native-DLL blind spots) —
  game.toml DEBUG toggle (revert before merge).
- `ws_backdrop_ring` — always-on rewrite ring (pc, kind, wcols, orig, final,
  extent, camx, count, a1/dl).
- `ws_backdrop_margin [m]` — live column-widen (proven irrelevant to the void).
- `ws_backdrop_stretch [on] [pct] [thresh] [mode]` — live 2D-stretch tuning + a
  per-frame dbg block (applied/prims/clearx/wide_cur/base/wide_w/off).
- Keyboard ALWAYS drives player 1 (OR'd with any controller; main.cpp) — debug
  convenience, unconditional.

# 2026-06-17 — CONTENT-MATCH DISPROVEN; writer chain mapped; SOURCE-SIDE chosen
The "BREAKTHROUGH — draw-time classification" / content-match plan above is the
THIRD dead end. Live RE (GP0-ring tools in TombaRecomp/tools/an_*.py):
- The cmd-0x25 flower template (a1=0x801a603c, generator 0x80116808) is TRANSFORMED
  before drawing: template clut 0x1c91/0x4bde, tpage 0x00a4 — NONE of those appear
  in any drawn prim (drawn flat-prim cluts all 0x7xxx); ~110KB of active template
  blobs collapse to a ~14KB drawn OT. No stable per-prim fingerprint exists.
- Flower prims are REBUILT into the OT each frame (DMA src = OT 0x0b3xxx, disjoint
  from struct 0x1a6xxx) → the committed address-range gate can never match (applied=0).
- The early-drawn 0x65 sprite GRID (6 cols x7, X=-2..318 step-64, cluts 780f/784f/798f,
  drawn before any 3D) is the leading void-layer suspect. Its XY is written via `sh`
  (X=0x13e=318) by SHARED helper 0x8005dfd8 (caller ra=0x8004e188), from a main-EXE
  builder at 0x8004e120-0x8004e188 that ALSO sprite-tags via 0x8005e08c → those
  grid sprites are SPRITE-TAGGED. So the fix likely belongs in the sprite-tag /
  native-wide path, NOT x_sites. (psx_ws_backdrop_x is squash-only; needs a
  native-wide STRETCH branch regardless.)
- TOOLING NOTE: wtrace `pc` is the STALE last-interp pc for native writes (codegen
  emits no g_debug_last_store_pc; only the interp sets it → all native OT writes
  read pc=0x80116f20). Trust the `ra` field (= jal+8). FUN_80027600 (GTE far-parallax
  emitter, the 8C un-squash target) writes polygons into the same OT buffer, so
  range-based wtrace is contaminated — use single-word traces (an_writer2.py).
- OPEN/DO-FIRST: VERIFY which drawn prims actually constitute the void (build a
  live prim<->pixel correlation: stretch/tint only a chosen {op,clut} or OT-range
  set, iterate vs wide_shot) BEFORE picking the source site. Then scale that layer's
  X at the source. Details + full reasoning in memory ws_backdrop_preload.md
  (2026-06-17 section).
