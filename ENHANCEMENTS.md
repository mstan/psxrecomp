# PSXRecomp — Enhancement-Tier Work (framework-wide)

Faithfulness is the foundation (CLAUDE.md Rule -1); this file tracks the
enhancement layer built on top of it: renderers beyond the software reference,
widescreen, load acceleration, etc. Per-game enhancement ideas live in each
game repo's ENHANCEMENTS.md. Active framework bugs referenced here live in the
game repos' ISSUES.md until a framework tracker exists.

---

## R1 — OpenGL renderer (2nd backend): PLAYABLE, flicker root-caused + fixed

**Status as of 2026-07-03** (branch `feat/renderer-finish`, working tree, uncommitted):

- The long-standing intermittent black-frame flicker (MegaManX6Recomp ISSUES.md
  #7 — the reason MMX6 shipped with the software renderer) is **root-caused and
  fixed**. Mechanism (proven via the new present ring + gl_coh_ring correlation):
  `flush_cpu_upload()` merged all pending CPU→VRAM writes into ONE union bounding
  box; a frame with two disjoint uploads produced a union spanning the display
  framebuffers, which the flush painted from the **stale CPU VRAM mirror** (the
  FBO is authoritative under GL) — stomping live frames with black. Two black
  presents per incident (one per double-buffer parity). Software renderer is
  immune (CPU array is authoritative there), which is why it was the safe default.
- Fix: exact pending-rect list (16 rects; merge only when zero uncovered pixels
  are added; wrap-aware GP0(A0) transfers split into up to 4 exact rects;
  overflow → order-preserving flush-all). Merge rule proven by a 20k-randomized-
  rect host unit test (0 stale / 0 missing painted pixels).
- New always-on observability: **gl_present_ring** (every SwapWindow site records
  path taken, src/letterbox rects, glGetError, wall-ms, backbuffer + blit-source
  pixel samples) — the instrument that made the 1:1 black-capture correlation
  possible. Plus a debug-server fix: send_fmt silently truncated >64KB responses
  into unparseable JSON (broke big ring dumps); now heap-formats exactly.
- Validation: ~18-minute MMX6 GL attract soak, ~1600 window captures across 3+
  full attract cycles — **zero isolated black frames, zero GL errors**, no other
  visual anomalies. Tomba1 build-gl rebuilt with the fix, boots clean (full
  Tomba1 attract soak still owed).

**Validation COMPLETE (2026-07-03, both titles):**
- MMX6: ~18-min attract soak, ~1600 window captures, zero isolated black
  frames (agent, ring-correlated).
- Tomba1: 24-iteration/720-capture attract soak (2 flags, both multi-frame
  FMV content cuts) + the definitive pass: 23,807 CONSECUTIVE presents
  (gapless, seq-verified) over a full attract cycle via gl_present_ring —
  ZERO isolated dark presents, zero GL errors. The capture-level flags do
  not exist at the swap level.

**Remaining to close R1:**
1. USER final validation at the MMX6 Rainy Turtloid standing-still spot (the
   original repro; MMX6 build-modern settings.toml is left on
   renderer="opengl").
2. Flip MMX6's shipping default software→opengl + close ISSUES.md #7.
3. The same union-upload bug exists in the Vulkan backend (see R2 item 1).

## R1b — Native-wide (16:9) GL: perf collapse ROOT-CAUSED + FIXED, band flicker FIXED

**Status 2026-07-03** (branch `feat/renderer-finish`, commits f5362f4..8b819eb):

- **The 16:9 perf collapse (Tomba2 3D attract 60→12fps, MMX6 2D attract dips)
  was never a GPU problem.** Stack-sampled (devkitPro gdb) in the wedge: the
  main thread lived in `ws_backdrop_site_kind` ← `exec_one` — under native-wide
  the dirty-RAM interpreter classified EVERY executed instruction as a possible
  backdrop rewrite site; the classifier rescans ±512 bytes on cache miss and its
  256-slot direct-mapped cache (2 hot PCs 1 KB apart collide) thrashed on
  overlay working sets. Squash mode gates the whole path off — that is why
  `ws_nw on=0` restored 60fps while every GPU theory (mirror FBO ping-pong,
  extra prims, present path) failed. The earlier "60ms scene GPU / 70us per
  prim" numbers were CPU-starvation-inflated GPU-timestamp gaps (the GPU idles
  between CPU-paced submissions inside the bracket) — treat GL timer numbers
  on a CPU-bound frame as suspect.
- **Fixes** (dbe7812 + 8b819eb): opcode pre-filter (only addu/or/addi/addiu can
  be rewrite sites) + 8192-slot full-PC-tagged caches + `g_dirty_ram_code_gen`
  invalidation (memory.c) + a per-entry SITE-WORD tag (revalidates the cached
  verdict against the live instruction word — plain-CPU-store overlay reloads
  never hit the page-marking hooks, and a stale verdict fires a GPR rewrite at
  the wrong instruction = guest corruption).
- **Numbers**: Tomba2 GL 16:9 attract (heavy scene, ~640-1100 prims) 72-95ms/frame
  → **17-21ms (p50 17.1ms, ~52-58fps)**; MMX6 GL 16:9 attract locked 16.7ms
  through demo stages (worst 10s window avg 25ms at stage-load transitions).
- **Top/bottom band flicker (MMX6 16:9, user-visible) FIXED** (a0b5843): the GL
  mirror pass scissored the FULL wide surface; the SW reference (`rt_wide`)
  only widens X and keeps the draw-area Y clip. Under MMX6's vertical double
  buffer (draw area alternates y=0/y=240, both bands in ONE wide surface)
  mirror draws bled across the band boundary and presented a frame late as
  edge flicker. Scissor is now full-width X / draw-area Y. Validated with
  0.15s-interval capture bursts: top/bottom 16-row inter-frame instability is
  0.26x/0.21x the scrolling middle band (edges quieter than content).
- **Wide surfaces now carry a DEPTH24_STENCIL8 attachment** (cfa79bb): the
  stencil-less wide FBO was a spec-gray target for the stencil-enabled mask
  fixup passes AND left the PSX mask-bit mirror silently no-op on the wide
  surface. (Its per-pass GPU "cost" measurements that motivated it were later
  shown starvation-inflated; the attachment stays for mask correctness.)
- **New permanent instrumentation**: frame_perf mirror split (GL_TIMESTAMP
  pairs per wide pass: mirror_gpu_ms / canon_gpu_ms / mirror_pass_us), CPU-side
  attribution (cpu_flush_ms, cpu_wide_ms, batches, wide target sets, wide FBO
  creations per frame), and `gl_ws_ablate mode=0..3` (skip mirror / state-only /
  no-FBO-rebind ablations) — the toolchain that exonerated the GPU and named
  the CPU producer.
- **MMX6 wedge incident (RESOLVED to one mechanism, poisoned overlay shards):**
  during the session MMX6 hit fatal wedges — twice in the 16:9 attract (wild
  dispatch 0x21010001 / 0x0C008096) and then DETERMINISTICALLY at boot frame
  2518 (unknown dispatch 0x80095098) at BOTH aspects. Timeline nailed it: the
  overlay self-heal wrote a fresh 001EA000 shard batch at 20:53, exactly when
  attract wedge #1 hit (shards hot-loaded mid-run); every boot after loads
  them and wedges at 2518; quarantining the batch
  (cache/.../cg4_0cec55ab.quarantine) restored clean boots + attract. Most
  plausible poison source: the INTERIM stale-verdict build (dbe7812 before the
  8b819eb word tag) corrupted guest RAM at 16:9, and autocapture snapshotted
  the corrupted overlay bytes into the captures the shards were compiled from.
  Self-heal recaptures with the hardened build. NOT a ws-stack or renderer
  defect. Tomba2 16:9 soaked clean throughout.

## R2 — Vulkan renderer (3rd backend): RENDERS GAMEPLAY AT SPEED, gaps cataloged

**Status as of 2026-07-03** (same branch/tree; `-DPSX_ENABLE_VULKAN=ON`, SDK
1.4.341.1; `feat/vulkan-renderer` turned out to be already merged into master —
only a 26-line build-guard needed salvaging from the retired _wt-vulkan worktree):

- Three bring-up bugs root-caused and fixed this session:
  1. **Boot wedge**: per-pixel GP0 uploads did 2 vkAllocateMemory + 2
     vkQueueSubmit each (driver churn → minutes-long stall, watchdog abort).
     Fixed with GL-style deferred batched uploads.
  2. **Shredded 3D**: draws raced CPU rewrites of the persistently-mapped vertex
     buffer (69.5% pixel divergence vs software at the same guest frame). Fixed
     with sub-allocation cursors + firstVertex bases.
  3. **Semi-transparency order violations** (59.5% divergence): VK still had
     GL's retired whole-batch STP split; ported GL's current two-pass model
     (ordered color pass + color-masked stencil fixup; semi prims isolated).
- Verified (guest-frame-aligned VRAM diffs via the new frameshot.py tool +
  window captures): title pixel-identical to software; attract within 0.62% of
  the GL oracle; **60.5 fps sustained**; vk_perf steady-state ~0-2 allocs and
  ≤6 submits/frame (was thousands). 24-bit FMV present path written (old one
  was provably black) but NOT yet verified on-screen.

**Gap catalog (ranked, low→high effort):**
1. ~~Port the exact-rect pending-upload fix from GL~~ DONE 2026-07-03: exact-rect
   list ported + VK-specific COALESCED flush (one staging pair + two submits per
   flush regardless of rect count; the naive per-rect port re-created the submit
   churn at 0.7 fps — VK pays per-submit where GL pays per-glTexSubImage2D).
   UP_RECTS_MAX=64 on VK so MDEC row-coalesced FMV frames fit in one flush.
   Verified: attract renders correctly at ~51 fps, vk_perf mostly-idle frames.
2. ~~Verify FMV on-screen~~ DONE 2026-07-03: Whoopee logo + intro CG movie render
   correctly on VK (window captures). NOTE Tomba2 movies are 15-bit MDEC->VRAM
   (upload path), NOT depth24 — the depth24 compose path remains no-regression-
   verified only; validate on a 24-bit title (MMX6 opening) later.
3. ~~Cache the FMV present staging image~~ DONE 2026-07-03: persistent image +
   mapped staging keyed by (w,h), freed on resize/shutdown (cpres cache).
4. ~~DS barrier~~ DONE 2026-07-03: explicit stencil-aspect self-barrier
   (late-tests write -> early-tests read|write) at every begin_geo_pass;
   layouts verified against the init transition chain + render pass
   (attachment-optimal throughout).
4b. NEW (minor): flush_cpu_upload allocates 2 stagings per flush — ~16/frame
   during MDEC FMV streaming only (~0 in gameplay). A sync-aware staging ring
   would zero it; low priority.
5. ~~Native-wide (16:9) compositor~~ DONE 2026-07-03 (0b23ea3): per-base_x wide
   surfaces (RGBA8 color + OWN stencil image + framebuffer on the SHARED render
   pass — every pipeline works unchanged, and the mask-bit stencil mirror is
   real on the wide surface from day one). The mirror is a second render pass
   appended to the SAME one-shot CB as each flushed batch (no extra submits);
   u_xoff/u_xhalf push constants (pre-plumbed in the shaders) carry the
   translation/wider clip. Mirror scissor = full-width X / DRAW-AREA Y (the GL
   band-bleed lesson applied from day one). wide_clear via ClearAttachments
   (color + stencil=bit15); full-screen overlay rects suppress the batch mirror
   and draw one full-wide-width rect (margins dim/fade). GPU-direct present
   blits the displayed band letterboxed at (4*wide_w : 3*native_w);
   vkb_render_wide_display readback backs the facade + debug dump. vk_perf
   gains wide/wclr counters. VALIDATED (Tomba2 build-vk, PSX_WS_FORCE_2D=1 —
   master Tomba2 has no sprite-tag hooks): 16:9 attract presents full-width
   (mine-cart demo captures), locks 60fps on normal scenes (frame_period p50
   16.68ms), ~51-57fps on the heavy semi-prim scene (29 wide passes/frame);
   4:3 unregressed (p50 18.1ms on the 452-flush semi-isolation scene — the
   pre-existing item-7 profile, wide counters 0). Margins show 4:3-culled
   geometry only — full margin content needs the cull-widened overlay cache
   (cg4_0cec55ab.ws-experiment, not installed on master).
6. SSAA scale >1 unvalidated on VK.
7. Semi-prim isolation perf (one draw per semi triangle; same cost as GL today).

**Validation targets:** MMX6 + Tomba2, agent does initial (window-capture series
+ frame-aligned cross-backend diffs), user does final.

---

## R3 — Validation sweep + merge (2026-07-03, USER-DRIVEN)

Full 3-title x 3-config playthrough validation (agent launched each config windowed
+ confirmed on-screen via window_capture; user drove input + gave the verdict):

| Title  | OpenGL 4:3 | Vulkan 4:3 | OpenGL 16:9 |
|--------|:----------:|:----------:|:-----------:|
| Tomba1 | PASS       | YELLOW     | PASS        |
| MMX6   | PASS       | YELLOW     | YELLOW      |
| Tomba2 | PASS*      | PASS*      | FAIL*       |

\* Tomba2 = experimental / not production-ready; not merge-blocking. (Vulkan 16:9
was intentionally skipped — user directive: all 16:9 in OpenGL only.)

Findings (tracked for the next work cycle):
- **OpenGL 4:3 is the strong, shippable path on all titles.** This is the win.
- **Vulkan 4:3 = YELLOW everywhere** (why it ships hidden/experimental):
  - Tomba2: sluggish FMV, minor speech-audio fidelity loss, in-game slowdown
    (possibly progressive), text boxes don't dismiss, HUD weapon icon renders in
    all 3 slots (only center should show).
  - Tomba1: minor horizontal bar artifact at top of the load screen; dwarf
    village sluggish (the known overlay-heavy scene; perf only, renders correct).
  - MMX6: significant slowdown in Rainy Turtloid's RAIN AREA even at 4:3 —
    VULKAN-SPECIFIC (on GL 4:3 it is subtle at most). Renders correct; perf only.
- **OpenGL 16:9:** Tomba1 clean PASS. MMX6 YELLOW (rain-area lag evident at 16:9;
  same Rainy Turtloid perf hot-spot). Tomba2 FAIL — widescreen does NOT engage
  (renders 4:3 pillarboxed in the wide window + slow); expected, master Tomba2
  has no sprite-tag ws hooks (ws work parked at 4:3).

**Policy gates confirmed already in-tree (no code change needed for the merge):**
- Vulkan is hidden by default: launcher offers only Software<->OpenGL
  (launcher.cpp:347/776); PSX_ENABLE_VULKAN defaults OFF (runtime.cmake:484);
  runtime downgrades renderer=vulkan -> opengl when not compiled (main.cpp:2463).
  Vulkan stays a dev/CLI-only backend (--renderer vulkan on a VK-enabled build).
- Widescreen carries an EXPERIMENTAL tag in the RmlUi launcher (launcher.rml:362).

**MERGED to master 2026-07-03** (runtime-only; codegen hash unchanged, no regen).
Game pins bumped to the new psxrecomp master. No release builds cut (user directive).

**Open follow-ups (next cycle):** VK perf (MMX6 rain-area, dwarf village, Tomba2
in-game — likely the CPU-bound dirty-RAM ws classifier path and/or VK per-submit
cost); VK correctness (Tomba2 persistent text boxes + 3-slot HUD icon; Tomba1 load
bar artifact); Tomba2 widescreen sprite-tag hooks; MMX6 Rainy Turtloid perf even on
GL 16:9; and the pending MMX6 shipping-default flip software->opengl to close
MegaManX6Recomp ISSUES.md #7 (GL validated this session).

---

## W1 — Tomba2 16:9: USER-FLAGGED visible issues queue (2026-07-06, NOT STARTED)

User-prioritized alongside the P0 crash work (ISSUES.md #8). None of these have
been investigated yet — this section is the work queue + every known lead.
All Tomba2, GL 16:9, worktree `_wt-tomba2-ipr` / `Tomba2Recomp` build-t2.

### W1.1 — Black background columns in 16:9 (P3; incl. beach area)

- **Symptom:** huge black regions where the 2D far backdrop should fill the wide
  margins (user Image 1: large black sky scene); also a ~24px black column in
  beach-village at game-x 85–109.
- **Class:** 2D far-backdrop doesn't cover the wide FBO edges.
- **Prior art / leads:**
  - `[widescreen.bg2d]` config (recompiler/src/config_loader.cpp:699).
  - Memory `ws_backdrop_preload.md` (Tomba1): far-backdrop void = early
    sprite-tagged 0x65 tile grid; fix = centre-stretch gated preload.
  - Memory `ws_draw_census_8c.md` (Tomba1 8C scene): void was GTE-3D driver
    FUN_8004db3c; fixed via depth-gated un-squash. The **8C draw-census ring**
    is the attribution instrument (always-on; query, don't arm).
- **Next step:** per-scene attribution — which producer draws the sky strips,
  and why the wide margins get no tiles. Attract may cover some scenes; the
  beach needs navigation (user drive or save).

### W1.2 — 4:3 object culling visible at wide edges (P2 cull widening)

- **Symptom:** objects pop in/out at the 4:3 boundary in 16:9 (world objects
  culled by game code against 4:3 screen extents).
- **Ghidra cull sites already identified** (tomba2_ram.bin; overlay addresses —
  validate per scene variant before wiring):
  - `FUN_8003e030`: sltiu 0x140 @ 0x8003E228
  - `FUN_80069b6c`: addiu +0xE6 @ 0x80069B84 + sltiu 0x1CD @ 0x80069B8C
  - `0x80110A08`: addiu +0x80 / sltiu 0x101
- **Fix shape:** `[widescreen.cull]` bias_sites/range_sites per-game config
  (enhancement-tier per-game shims are legitimate here). Prior art: Ape
  bring-up used per-game cull imms (0x181) + signed idioms (slti/bltz)
  (memory `ape_widescreen_bringup.md`).

### W1.3 — Dialogue-box text tearing in 16:9 (P4)

- **Symptom:** full-width dialogue text splits with gaps (user Image 2: "Water
  cam…e out from th…e faucet").
- **Cause known:** gpu.c `ws_nw_hud` thirds — left/right-third sprites are
  anchored apart for HUD proportion; full-width dialogue glyphs tear at the
  third boundaries. Needs a smarter anchor (e.g. detect wide text rows /
  dialogue-box association) rather than blanket thirds.

### W1.4 — Beach-area framerate (P1 perf; also user-flagged)

- 2D isometric scenes run 0.42–0.70× (beach/village worst). The convergence
  blockers are FIXED (autocapture futility backoff + entry-based coverage in
  tools/compile_overlays.py — see handoff 2026-07-06); the campaign was
  interrupted by ISSUES.md #8 and should resume after it: expect 2D scenes to
  climb past 0.85 as coverage converges. Residual axes if short of ~1.0:
  per-block pump overhead (psx_check_interrupts ~2.6M calls/s), attract
  cycling. Measure ONLY with two freeze_check snapshots over a known wall
  interval (executed throughput = d(psx_cycle_count) − d(cycles_skipped));
  phase_profile is a ring read and returns instantly.
- CAUTION: any pace numbers taken in diff mode before ISSUES.md #9's redesign
  lands are garbage (the wedged shadow silently disabled all native dispatch).

## W2 — Tomba 1 (SCUS-94236) 16:9: HUD at the true wide corners (DEFERRED 2026-07-10)

User ask: in native-wide 16:9, re-anchor the HUD to the wide corners
(repositioned, never stretched). First attempt shipped and was REVERTED the
same day (game.toml `nw_hud_corners` back to false; the framework machinery
stays, inert + A/B-able). What was learned, so the next attempt starts ahead:

- **Mechanism reused:** `[widescreen] nw_hud_corners` (gpu.c `ws_nw_hud_shift`
  thirds translate, from the MMX4/5 campaign) + a new tag-title scoping: polys
  and lines never shift (world/characters), rect-family prims shift only when
  UNTAGGED. TCP `ws_hud_mode {"tag_rects":0|1}` A/Bs the tagged-rect gate live.
- **What worked:** vitality gauge + life-counter (untagged rects, fully inside
  one outer third) anchored flush to the wide corners, world untouched.
- **Failure 1 — composite tear (same class as W1.3):** the in-world dialogue
  box ("It's locked...") is a composite of untagged rects SPANNING zone
  boundaries: its left/right end caps sit in the outer thirds and get pulled
  to opposite screen edges while the text stays centred — the box visibly
  splits. A blanket per-prim thirds rule cannot ship; it needs composite-group
  awareness (e.g. group prims drawn adjacently in the packet arena / same OT
  bucket, shift a group only if the WHOLE group fits in one zone).
- **Failure 2 — AP counter immobile:** the AP composite (0x65/0x67 rects,
  x≈220-292, y≈8-12) renders through the TAGGED sprite funnel (0x8005E08C),
  so the untagged-only gate skips it. Census now records a `tagged` column
  (gpu.c WsCensusEntry) — verify with `ws_census`. Lifting the gate via
  ws_hud_mode shifts it, but then tagged world-anchored rects (collectible
  sprites) near edges would shift too — needs the same group/HUD-band
  discrimination as Failure 1 or a per-title HUD packet arena range
  (`nw_left_hud_packet` exists for exactly this; needs Tomba's HUD arena
  addresses from a census session: HUD prims live in the 0x000Bxxxx/0x000Cxxxx
  double-buffered packet arenas alongside everything else, so the range must
  come from finer addresses).
- **Groundwork landed on `feat/ws-2d-scene-pillarbox`:** census `tagged`
  column, `ws_hud_mode` live A/B, and the untagged-rect scoping that makes
  `nw_hud_corners` safe to experiment with on tag titles.
