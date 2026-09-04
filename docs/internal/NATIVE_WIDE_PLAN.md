# Native-Wide Rendering — High-Level Plan (handoff)

Branch: `feat/native-wide` (both repos), forked from `feat/widescreen`
(psxrecomp `b1b7d6b` / TombaRecomp `36dcc42`). Tomba's `psxrecomp-v4.pin`
on this branch points at the psxrecomp `feat/native-wide` tip.

> **For the next session:** this is a *direction + phased plan*, not a spec.
> Read the "Current state" and "Core idea" sections, then execute the phases
> in order, verifying each against the oracle before moving on. The completeness
> rule applies: native-wide **replaces** the squash hack — do not ship both
> side by side. The last phase deletes the hack.

---

## Implementation status (2026-06-15)

**Foundation implemented + verified (runtime-only, committed on this branch).**
The centering mechanic adopted is the **proven `draw_offset`-shift** approach
(not the `OFX`-bias framing the phases below were originally written around — see
"Core idea" for why it's strictly simpler). A wide-aspect **mode** model now
lives in `gpu.c`: `0 = off (4:3 identity)`, `1 = squash (legacy)`, `2 =
native-wide`. Native-wide, on a game frame (not FMV/menu):

- **GTE** is fed the 4:3 ratio → identity, **no squash** (`main.cpp` engage).
- **`draw_offset_x += OFFSET`** at the GP0(E5) chokepoint (`gpu.c`) → 3D *and* 2D
  shift together by `OFFSET = round(W*(3num-4den)/(8den))` (=53 @ 320/16:9).
- **`draw_area_right += EXTRA`** at GP0(E4), **clamped to 1023** so a buffer at
  the VRAM edge clips rather than wraps into textures (`EXTRA = 2*OFFSET = 106`).
- **Present widens the display read by EXTRA** (`main.cpp` `present_w`); the
  existing 2× present then maps it 1:1 (isotropic) — the old squash-stretch
  becomes an honest 2× with no magnification. No renderer-backend change needed
  (both already render full-VRAM-width; only the read width + offsets changed).
- Diagnostics: `gpu_state` reports `ws.mode`/`ws.nw_extra`; **`ws_nw on=<0|1>`**
  TCP command toggles native-wide vs squash live for A/B.

**Verified in gameplay (16:9):** content renders at **correct native
proportions** (A/B vs squash: squash visibly stretches the dialogue text; native-
wide keeps it compact) and present is 1:1. 4:3 is identity **by construction**
(mode 0 → every native-wide term is 0; `present_w == w`).

**Decision point #1 RESOLVED with ground truth (the gating finding).** Tomba's
VRAM double-buffer is **two *adjacent* 320-wide buffers**: front `x∈[384,703]`,
back `x∈[704,1023]` — the back is **flush against the 1024 VRAM right edge**, and
the two buffers touch (704 = 384+320). So there is **no horizontal headroom** for
the +EXTRA widening on either buffer: widening the back wraps past x=1023 into the
texture region; widening the front overlaps the back. The revealed margin strips
therefore show **adjacent-buffer / VRAM-wrap bleed** (a duplicated scene fragment
on the right), NOT the clean "stale VRAM" the earlier PoC notes assumed. The
center is correct; the edges are not.

**The flicker (why the shared-canvas approach fails).** The first cut shifted +OFFSET
into `draw_offset` and widened the display read by +EXTRA *inside canonical VRAM*.
This **flickers badly** (and even segfaults) and cannot be made clean, because:
- The display **alternates between the two framebuffers every frame**. Buffer A's
  widened read `[384,810]` is in-bounds (→ bleed strip), but buffer B's `[704,1130]`
  **falls off the 1024 VRAM edge** → black strip + the draw wraps `[1024,1129]→[0,105]`
  over textures/CLUTs (the idle segfault). Two totally different frames → flicker.
- Even widening VRAM to 2048 does **not** fix it: the buffers are *adjacent*
  (Δ=320), so widened-A `[384,809]` and widened-B `[704,1129]` **share storage**
  at `[704,809]`. A's content `[437,756]` and B's `[757,1076]` are adjacent → each
  buffer's margin reads into the *other's* content, and clearing one margin erases
  the other's content. Unfixable in a shared canvas.
- VRAM is ~95% full (occupancy-mapped) → no room to *relocate* buffers apart.
- Changing the canonical wrap mask to `&2047` is also unsafe: it alters PS1
  coordinate semantics (negative/clipped coords, VRAM copies, clears, mask ops)
  for the opted-in game, not just our framebuffer draws.

**Adopted architecture (separate wide compositor surface).** Keep canonical PSX
VRAM **1024-wide and completely faithful** — every GPU op unchanged, so 4:3 / non-
opted games are byte-identical (the extensibility guarantee). For an opted-in wide
game, maintain **independent wide present-surfaces keyed by framebuffer base**
(learned from the `display_x` set). **Mirror** each framebuffer-targeting primitive
into its buffer's surface at local coords `(SX+OFFSET, SY)`; clear margins
in-surface; present from the surface matching `display_x`. Textures/CLUTs always
sample canonical VRAM; the wide surfaces are render targets, not texture memory.
Independent surfaces ⇒ no A↔B sharing ⇒ no bleed, clean margins, no flicker.

Milestones: (1) wide surface + mirror + present → flicker/bleed gone (margins
blank until culls widen); (2) regen-class cull-widening submits the off-screen FOV
→ it rasterizes into the margins → true native-wide. Key complexity driver:
whether the game does framebuffer feedback (reads its framebuffer back / VRAM→VRAM
copies between buffers); `gp0_copy` is nonzero for Tomba so worth confirming.

Reusable from the first cut: the opt-in **mode descriptor**, GTE-identity feed,
`ws_nw_offset/extra` math, `ws_nw` live toggle, debug fields. Reverted: the
canonical `draw_offset`/`draw_area`/clamp injections (they move into the wide
surface's local coords). Files: `runtime/src/gpu.c`, `runtime/include/gpu.h`,
`runtime/src/main.cpp`, `runtime/src/debug_server.c`, `runtime/src/gpu_sw_renderer.c`,
`runtime/src/gpu_gl_renderer.c`.

---

## Goal

Render Tomba (and the framework generally) at a genuinely wide internal frame
(16:9 / 21:9) with **correct proportions everywhere and no magnification**,
replacing the current squash-then-stretch hack. The extra horizontal field of
view should be *real rendered pixels*, not a blow-up of a 4:3 frame.

## Why (what's wrong with what ships today)

`feat/widescreen` implements the **squash hack**:

1. **Squash** — `gte_set_display_aspect()` in `runtime/src/gte.cpp` multiplies
   projected screen-X by `(4·den)/(3·num)` (`s_ws_xnum/s_ws_xden`), cramming more
   world into the native ~640-wide internal frame.
2. **Stretch** — the present path in `runtime/src/main.cpp` (`g_logical_w`,
   `SDL_RenderSetLogicalSize`, the `dst` rect around line 1339) stretches that
   640-wide frame out to the wide window.

Squash-then-stretch cancels for true 3D geometry → wider FOV, but it is
structurally lossy and spawns a pile of compensations that all exist *only* to
fight the stretch / squash:

- 2D screen-space (HUD, sprites, FMV, parallax backdrops) is computed without the
  GTE, so the present-stretch distorts it → `hud_sprt_squash`, per-prim
  proportion correction, FMV pillarbox, `psx_ws_backdrop_x` screenX squash.
- Backdrops authored for a 4:3 extent fall short of the revealed wide edges
  → the 8C blue void, patched by the **depth-gated un-squash**
  (`s_ws_suppress`, `s_ws_far_threshold = 900`, the per-vertex SZ split in
  `gte.cpp`).
- It's a magnification → softness.

Native-wide removes the *cause*, so most of the above compensations can be
deleted rather than tuned.

## Core idea

Make the runtime render into an **actually wider framebuffer** and stop
squashing / stretching:

- Internal frame width grows from the 4:3 `W` to `W · (num/den)/(4/3)` (e.g.
  ×1.333 for 16:9), height unchanged.
- The **GTE projection is identity-scaled** (no X-squash); instead the
  projection center X (`OFX`-equivalent, the `s_ws_*` bias) is shifted so the
  3D scene is centered in the wider frame. The extra width reveals world that
  was previously off-screen left/right — true FOV.
- The GPU draw area / clip / scissor and the display region widen to match.
- Present is **1:1** (no horizontal stretch); the window's logical width equals
  the wide internal width.

### The hard truth (scope driver)

3D geometry adapts for free via the projection center shift. **2D
screen-space elements do not** — anything the game positions in raw screen
coordinates (HUD, dialogue boxes, the parallax 2D backdrop layers, menus) will
cluster at the left and leave the new right-side blank. So native-wide still
needs **per-game 2D re-anchoring** — but the goal flips from "de-stretch" to
"re-anchor / widen-layout with correct proportions," which is cleaner and reuses
the existing Ghidra-evidenced site lists in Tomba's `game.toml`
(`[widescreen]`, `[widescreen.cull]`, `[widescreen.backdrop]`).

---

## Phased plan

Each phase is independently verifiable. 4:3 must remain pixel-identical
throughout (the wide path is gated; identity when `num/den == 4/3`).

**Phase 0 — One wide-viewport abstraction.**
Introduce a single descriptor (wide internal width, projection-center bias,
present mapping) consumed uniformly by GTE, GPU, and present. Replace the
scattered `s_ws_xnum/xden` + `g_logical_w` stretch with this. Keep 4:3 = identity.
Files: `gte.cpp`, `gpu.c` (`gpu_ws_configure`, `gpu_ws_present_native_43`),
`main.cpp`.

**Phase 1 — Widen the render target (both renderers), present 1:1.**
- Software: widen the VRAM display/draw region and the `g_hr` SSAA mirror
  (`gpu_sw_renderer.c`); present without horizontal stretch (`main.cpp` dst rect).
- OpenGL: widen the VRAM FBO + viewport (`gpu_gl_renderer.c`, note ~line 1115).
- *Checkpoint:* with no GTE change yet, the game renders into the left portion
  with a blank wide margin — proves the pipeline widened and present is 1:1.

**Phase 2 — GTE: projection-center shift, identity scale.**
Remove the X-squash; add a horizontal projection-center bias so 3D centers in
the wide frame. Now 3D fills the wider frame with correct proportions and real
extra FOV. Delete `s_ws_xnum/s_ws_xden` and the depth-gated un-squash
(`s_ws_suppress`, `s_ws_far_threshold`) — native-wide makes them unnecessary.
*Checkpoint:* 3D world correct & wide; 2D still left-clustered.

**Phase 3 — GPU draw offset / clip / scissor for the wide frame.**
Make drawing-area, clip rects, and scissor respect the wide width so 3D prims
and clipping aren't cut at the old 320/640 boundary (`gpu.c`,
`gpu_sw_renderer.c`, `gpu_gl_renderer.c`).

**Phase 4 — 2D screen-space re-anchoring (the per-game bulk).**
Repurpose Tomba's existing site lists from "squash" to "re-anchor":
- HUD/sprite tags (`[widescreen] sprite_tag_funcs`, `hud_sprt_squash`) →
  edge-anchor instead of squash.
- Parallax backdrops (`[widescreen.backdrop] x_sites`, `psx_ws_backdrop_x`) →
  widen the layout span instead of squashing screenX.
- Dialogue / menus → center or edge-anchor.
This is the long tail; drive it by the Ghidra-evidenced PCs already documented.

**Phase 5 — Culling widening relative to the new half-width.**
The `[widescreen.cull]` bias/range sites (`psx_ws_x_margin`) already widen the
world-space cull; recompute the margin against the new wide half-width so far
geometry isn't culled before it reaches the wide edges. Confirm the 8C void,
edge-drift, and grass-top straddle are gone (they should be, structurally).

**Phase 6 — 4:3 content in a wide frame.**
FMV (4:3 source) and the BIOS boot pillarbox/center in the wide frame; menus
that must stay 4:3 likewise. Reuse the existing FMV-4:3 / BIOS-4:3 logic.

**Phase 7 — Delete the squash hack (completeness).**
Once native-wide reaches parity, remove the squash codepath entirely:
`s_ws_xnum/xden`, the present-stretch (`g_logical_w` stretch semantics),
`gte_set_display_aspect` squash math, `psx_ws_backdrop_x` squash, the
depth-gated un-squash, and the now-dead compensation patches. Update
`../WIDESCREEN.md`. No dual-mode left behind.

---

## Decision points for the handoff session

1. **Internal width vs VRAM.** PSX VRAM is 1024×512; a 16:9@240p wide frame
   (~853 at 2× internal) fits, but confirm the display-region/`DISPENV`-width
   override approach and how it interacts with the game's own GPU display reg
   writes. Decide: runtime overrides display width, game keeps drawing.
2. **Projection-center bias source.** Per-frame from camera/projection state, or
   a fixed center shift? Start fixed-center (symmetric reveal); revisit if the
   game re-centers projection mid-scene.
3. **2D re-anchoring coverage.** How much of the long tail (every UI/backdrop
   site) is in scope for first landing vs. follow-up. Recommend: core HUD +
   backdrops first, log uncovered sites (no silent gaps).
4. **Single wide mode vs. keep squash as fallback.** Recommendation: native-wide
   *is* the wide mode; 4:3 stays identity; squash is deleted (Phase 7). Do not
   ship both.

## Verification

- Per phase: oracle screenshot compare + the `tools/capture_game_window.ps1`
  client-area grab (note: this build's window title is "Tomba! Recompiled", so
  match `*Tomba*`, not `TombaRecomp*`).
- 4:3 regression gate: with `aspect_ratio = "4:3"` the output must stay
  pixel-identical to master at every phase.
- Known-issue sites to confirm fixed by Phase 5: 8C far-backdrop void,
  edge object-pop/drift, mountain grass-top straddle, and likely 8F
  dialogue-split.
- `ws_census` / `ws_margin` / `ws_far_threshold` debug-server commands
  (port 4470) remain useful for inspecting per-draw extents while bringing this up.

## Build / run notes for the next session

- Both repos are on `feat/native-wide`. Tomba's `psxrecomp-v4.pin` points at the
  psxrecomp `feat/native-wide` tip; build with `tools/build-linux.sh --nopin`
  (or check the psxrecomp tip out so HEAD matches the sha).
- Regen Tomba C after recompiler-side changes:
  `recompiler/build/psxrecomp-game.exe --config game.toml` (run from the Tomba
  repo root). `regen.ps1` is broken — invoke the exe directly.
- Canonical build dir: `TombaRecomp/build-stable` (Ninja, RelWithDebInfo, TCP
  debug on 4470). Build target `psx-runtime`.
- Reference: the squash-hack equivalents of every symbol above live on
  `feat/widescreen`; diff against it to see what each compensation was doing.
