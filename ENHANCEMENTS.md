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
- Vulkan is hidden by default: the shared launcher offers only
  Software<->OpenGL; PSX_ENABLE_VULKAN defaults OFF (runtime.cmake);
  runtime downgrades renderer=vulkan -> opengl when not compiled (main.cpp:2463).
  Vulkan stays a dev/CLI-only backend (--renderer vulkan on a VK-enabled build).
- Widescreen carries an EXPERIMENTAL tag in the launcher.

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

## L1 — Load-time-toward-0 burndown (2026-07-14, ACTIVE — guinea pig: Tomba 1)

Full analysis + gates/kill criteria: `docs/LOAD_TIME_ZERO.md` (this branch,
`spike/load-time-zero`). ChatGPT consult merged (thread "PSXrecomp
workspace"). Already-settled items are NOT re-tried: turbo_loads (shipped,
~2x), disc_speed divisor 4x/instant (proven unsafe — MMX6 VSync-callback
wedge), yield pumps r1/r2 (proven fatal — green-thread corruption), BIOS CD
HLE (rejected — no landmine, no host win). The frame:
`wall = guest_time x host_cost_per_guest_sec`; under turbo the window is
emulation-throughput-bound, so the safe axis is host cost (multiplies with
turbo), the risky axis is guest time.

**Prioritized burndown (most agnostic + most likely beneficial first):**

- [x] **L1.0 — E0 `load_probe_v2`: load-window decomposition on Tomba 1.**
  100% agnostic, zero risk, prices every bet below. Split a real pig-load
  window into guest time (seek / sector cadence / per-sector processing /
  explicit waits) and host time (native code / decompressors / interp /
  CD-event machinery / SPU / GPU / pump). Existing rings first
  (freeze_check, cdrom_bursts, dirty_ram_stats per_pc, phase_profile);
  extend rings only where attribution is blind. Decisive question: why
  only ~2x during a presentation-suppressed window? Thresholds: decomp
  ≥~40% host → shards serious; ≤~8% → kill shards; CD/event/SPU machinery
  dominates → L1.2; wall-clock limiter found → fix that first.
- [ ] **L1.1 — Turbo hardening.** (a) re-validate SDL pump under a live
  burst (fix appears in-tree: pump precedes the turbo early-return);
  (b) audio at the HOST SINK only (drop excess samples, crossfade on
  exit; never guest state); (c) root-cause MMX5 dev-tools+turbo 0xE10
  boot wedge (foundation timing bug).
- [x] **L1.2 — Event-horizon acceleration + batched device ticking.**
  Provably side-effect-free poll/idle regions jump to the next scheduled
  observable event with exact cycle credit + identical event ordering;
  devices advance to deadlines instead of per-block ticks. Attacks the
  ~2x ceiling directly; class-level, all titles inherit. Gate set from
  L1.0's poll/idle share. Shipped: deadline-based device servicing, six Tomba
  wait sites, and proof-gated generic idle skipping. Default-off cross-game
  smoke validation passed on MMX5 and MMX6. Kill: <10% gain or ONE event-order
  divergence.
- [x] **L1.3 — Load-path overlay coverage (killed by gate).** L1.0 measured
  zero in-window interpreter instructions, so there is no coverage win to buy.
- [x] **L1.4 — Data shards (rejected/quarantined): verify-only SHADOW
  mode, then replay.** Gated on L1.0 (decomp ≥~20-25% host share).
  Correctness bar: temporal write visibility — replay sound only if
  IRQs-off across the window OR duration < next observable event.
- [x] **L1.5 — Authentic drive backlog (killed as acceleration).** Passive
  deadline-vs-exposure probe measured 1,304/1,304 data sectors available on
  their exact scheduled cycle, then the intentional fixed 5,000-cycle INT1
  presentation delay. Zero early/late sectors, holds, pending/lost INT1s, or
  overwrites: there is no artificial lateness for backlog/catch-up to remove.
- [x] **L1.6 — Seek-only latency probe (killed on Tomba).** The measured
  New Game window issued zero seek commands. Read-start latency was only
  7,676,928 / 298,130,657 cycles (2.57%); pause latency was 8.11% but is an
  authentic CPU-visible ordering contract, not a safe seek-speedup target.
- [ ] **L1.7 — Phase-2 doors (open only with cause):** per-title read
  speedup with XA/CDDA/MDEC exclusions; decompressor HLE (only via L1.4
  failing for a named reason); load-transition state cache (the only
  true near-zero; needs thousands-of-frames differential validation).

**Live status / decision ledger (Tomba 1, 2026-07-14):**

| Item | Verdict | Evidence / measured result |
|---|---|---|
| L1.0 decomposition | DONE | 761 sectors, ~9.5 s baseline window; zero in-window interp; host/static execution dominated. |
| L1.1 turbo hardening | IMPLEMENTED ON TOMBA; CROSS-GAME SMOKE PASSED | SDL pump remains before every turbo return; 4-frame engage + 6-frame release debounce passed live QA. Opt-in host-audio sink advances canonical SPU state while discarding only accelerated host output; Tomba listening QA passed after 1,100,752 discarded SPU frames. MMX5 and MMX6 debug-tools builds booted through loads into live gameplay with normal visuals/audio; the historic intermittent MMX5 0xE10 root cause remains a separate long-run investigation. |
| L1.2 event horizon | IMPLEMENTED; CROSS-GAME SMOKE PASSED | Production cycle advancement already batches device service at event/MMIO deadlines. Six configured PsyQ CD-wait sites delivered ~27% + ~9% stages. Generic idle-loop skip then cut warm bursts 0.53->0.34 s and 2.19->1.73 s, with 4,599 skips / 765M guest cycles and zero CD overwrites. Strictly per-game opt-in; MMX5/MMX6 exercised the default-off compatibility path successfully. |
| L1.3 overlay coverage | KILLED | Interpreter share was zero in the measured load window. |
| L1.4 asset/data replay | REJECTED FOR NOW | `FUN_8003EF50` replay produced title/game texture corruption despite zero verifier failures: v1 temporal verifier is unsound. Data shards default off and artifacts removed. |
| Configurable warm CD routes (L1.7 read-speed branch) | ACCEPTED, STRICTLY PER-GAME OPT-IN | Framework accepts up to 16 strict LBA routes with mismatch fallback and consumer-paced IRQ/DMA. Only data-read cadence accelerates; XA/CDDA, seek, and motor timing remain authentic. Tomba multi-route regression: 3 matches, 1,944 accelerated sectors, zero overwrites. Legacy singular config is deprecated. |
| L1.5 authentic backlog | DONE / KILLED AS ACCELERATION | 1,304/1,304 sectors exact-deadline; INT1 exposure exactly +5,000 cycles; zero holds, pending/lost, or overwrites. |
| L1.6 seek-only probe | DONE / KILLED ON TOMBA | Automated New Game: 0 seeks across 792 data sectors. Read-start latency was 2.57% of the data span, below the 10% gate. Pause was 8.11% but remains authentic because early completion is a known race/wedge class. |
| L1.7 state cache / broader HLE | DEFERRED | User excludes savestates; decompressor replay failed correctness. |

Method, every experiment: measure first via always-on rings; start flag-gated;
one per session; kill criterion written before code. `idle_skip`, warm CD routes,
and the turbo host-audio sink are all strictly per-game opt-in. MMX6 remains the
next deeper corpus/soak target after its successful boot/load/gameplay smoke.

---

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

---

## G1 — Sub-pixel vertex precision + perspective-correct textures (issue #92)

PS1 polygon jitter ("wobble", "bouncing lines") and warped floor/wall textures
have one root cause each, both in the fixed-point geometry pipeline:

- **Jitter.** The GTE computes the projected screen position in 16.16, then
  saturates it to an integer pixel when it pushes SXY. The fraction is thrown
  away. A slowly moving mesh therefore snaps its vertices between whole pixels
  and the model shimmers.
- **Texture warp.** The GPU interpolates UV *affinely* across a triangle, with
  no 1/z term, so a large floor or wall polygon's texture swims as the camera
  moves.

Both are addressed as **opt-in, visual-only** enhancements. The PS1-visible GTE
SXY FIFO stays integer and fully faithful — a game's own post-projection
screen-bounds culls and any SXY readback see exactly what hardware produces.
Nothing about guest state changes; the correction lives entirely on the host
render path.

### Configuration

```toml
[video]
geometry_correction   = true   # sub-pixel vertex precision (kills the wobble)
perspective_texturing = true   # perspective-correct UVs on world polygons
supersampling         = 2      # REQUIRED for geometry_correction to be visible
```

Both default **false** (the faithful floor). They are independent — a title may
want stable geometry without changing texture mapping. Settable per-game in
`game.toml` and per-player in `settings.toml` (the player's file wins, and a
launcher save round-trips both keys rather than dropping them).

`geometry_correction` needs `supersampling >= 2`: at native resolution the
corrected position rounds back to the pixel it started on, so there is nothing
to see. The runtime prints a note at startup when it is on at scale 1.

### How it works

The recompiler emits GTE commands as calls to a single runtime entry point
(`gte_execute`), which both the compiled backend and the dirty-RAM interpreter
share — so unlike an interpreter/dynarec emulator there is no dispatch hook to
add, just one funnel to instrument.

1. **`runtime/src/gte.cpp`** — RTPS/RTPT keep the discarded 16.16 fraction in a
   side cache keyed by the packed SXY word it rounded to (`geom_note`).
   Saturated (off-screen) projections are rejected: they carry no usable
   sub-pixel information.
2. **SWC2 provenance** — the recompiler, strict translator, dirty-RAM interp,
   overlay ABI (v14) and fallback interp all call
   `gte_precision_store_word(addr, reg)` when a projection register is stored to
   guest RAM, recording *which RAM address* a projection landed at. Perspective
   texturing only fires when all three of a triangle's position words came from
   such a store at that exact DMA packet address — which preserves the
   association through ordering-table reordering and rejects CPU-built UI and
   2D sprites outright. A plain `sw` to a tracked address invalidates it.
3. **`runtime/src/gpu.c`** — `prepare_precise_triangle()` /
   `prepare_texture_triangle()` look the packet up per triangle and hand the
   result to the renderer facade as sideband state for the next draw
   (`gr_set_precise_triangle` / `gr_set_perspective_triangle`).
4. **All three renderers consume it.** Software uses the fractional positions
   in its supersampled mirror; OpenGL and Vulkan take them as float vertex
   positions directly. For perspective UVs both GPU backends carry a per-vertex
   `a_q` weight and emit clip coordinates pre-multiplied by `w = 1/q`, so the
   hardware's own perspective divide interpolates a `smooth` UV varying while
   the affine `noperspective` one stays available. **`a_q == 0` (the default)
   makes `w` exactly 1.0 and selects the affine varying — the pre-feature
   pipeline, unchanged.**

Save states and speculative native-validation passes drop host-only provenance
(`gte_precision_timeline_invalidate`, `gte_precision_speculative_begin/end`) so
a rewind can never resurrect a stale projection.

### Validation story

By construction this feature *diverges* from stock hardware output, so the
Beetle oracle cannot be the judge of the corrected frame. What the oracle still
pins is the part that must not move: **with both flags off the output is
byte-identical to the pre-feature build**, and guest-visible GTE state is
identical either way (the SXY FIFO is untouched in both). That reduces
validation to (a) an off/off pixel-identity check against the oracle, and
(b) human A/B of the on/off frames on a 3D title.

`gte_geometry_correction_hits()` and `gpu_texture_correction_hits()` report how
many vertices/triangles were actually corrected — the "is this doing anything
on this title" counter, and the thing to check first when a title shows no
visible change.

### Provenance

The GTE side cache, SWC2 provenance tracking, GP0 triangle preparation and the
software-renderer consumption path were contributed by **Kareem Olim (kem0x)**
in [PR #14](https://github.com/mstan/psxrecomp/pull/14) and parked in commit
`2ceaf5a` (see `docs/internal/upstream/kem0x-pr14-projection-perspective.md`),
disabled pending generic setters. This work adds the opt-in configuration, the
renderer-facade seam, and OpenGL + Vulkan support.

### Status / next

- **Done:** config plumbing (game.toml + settings.toml + launcher seed
  round-trip), renderer-facade sideband, software / OpenGL / Vulkan consumption,
  `[video]` plumbing unit test.
- **Open:** per-title A/B validation. Ape Escape is the obvious first 3D
  subject (Tomba 1/2 and MMX5/6 are largely 2D, where neither knob does much).
- **Open:** launcher (recomp-ui) toggles. The keys round-trip through
  `settings.toml` today, but there is no UI row yet — that lives in the
  recomp-ui repo.

### G1.1 — MEASURED REGRESSION: partial coverage cracks meshes (2026-08-05)

**User verdict on Ape Escape: "little lines jittering everywhere — visually
this is worse."** Confirmed and root-caused. `geometry_correction` must not be
presented as usable in its current form.

Measured on Ape Escape (OpenGL, supersampling 2, 213-frame window, via the new
`geom_correction` TCP command):

| | per frame |
|---|---|
| GP0 draw commands | ~316 (≈400–600 triangles) |
| vertices given sub-pixel positions | ~114 (≈38 triangles) |
| triangles given perspective UVs | ~1.9 |

**Under 10% of the scene is corrected.** `prepare_precise_triangle` is
all-or-nothing per triangle, so every boundary between a corrected triangle and
an uncorrected neighbour is a seam: one edge moved sub-pixel, the other stayed
on the integer grid. Because the geometry cache is direct-mapped and keyed on
the *rounded* position, which triangles win changes frame to frame — so the
seams move. That is exactly the reported jitter.

### G1.2 — What the reference implementations actually do

Both vendored emulators (`beetle-psx/pgxp/`, `duckstation/src/core/cpu_pgxp.cpp`)
implement PGXP the same way, and it is **not** what is parked here:

1. **A complete shadow of the dataflow.** A `PGXP_value {x,y,z,flags,value}` per
   32-bit word of RAM + scratchpad (Beetle mirrors all 2 MB; DuckStation the
   same), plus a shadow per CPU GPR and per GTE register.
2. **Propagation through every instruction.** Beetle registers **47 CPU hooks** —
   LW/LH/LB/LWL/LWR, SW/SH/SB/SWL/SWR, ADD(I)(U)/SUB(U)/AND/OR/XOR/NOR/SLT(U),
   SLL/SRL/SRA(+V), MULT(U)/DIV(U), MFHI/MTHI/MFLO/MTLO, LUI. DuckStation carries
   the same set. High precision therefore survives any route the game takes from
   GTE output to the GP0 packet.
3. **`Validate(value)`.** Every shadow read checks the tracked `value` against
   the *actual* current word and drops the shadow on mismatch. This is what stops
   stale precision from corrupting geometry.
4. **The value-keyed cache is only a LAST-RESORT FALLBACK**, and even then it is
   fully direct-indexed — Beetle `vertexCache[0x800*2][0x800*2]`, DuckStation
   2048×2048 — so distinct screen positions **never collide**; it is gated on an
   ambiguity flag (`gFlags == 1`, "only one value was recorded at this position")
   and it disables perspective (`valid_w = 0`) because its w is untrustworthy.

**The parked implementation is only item 4, degraded**: an 8192-entry *hashed*
table (unrelated positions collide) with *no* ambiguity check and *no* primary
path. A vertex can therefore inherit a different vertex's fraction. That is the
design defect, not a wiring bug.

### G1.3 — What matching them costs in a static recompiler

The emit mechanism already exists — `gte_precision_store_word(addr, reg)` is
emitted at SWC2 sites today — so this is an extension, not new machinery:

- per-word shadow of RAM + scratchpad (~12 MB), per-GPR and per-GTE-reg shadows;
- propagation hooks at ~47 instruction classes, emitted in `code_generator.cpp`
  and `strict_translator.cpp`, mirrored in `dirty_ram_interp.c` and
  `psx_interpreter.c`, and forwarded through the overlay ABI (another bump);
- `Validate()` on every shadow read;
- GPU-side lookup keyed on the packet word with a `value ==` check, with the
  corrected fallback cache last.

**The static-recompiler-specific cost:** in an interpreter these hooks are a
runtime branch. Here they are emitted C on the hot path, so they bloat generated
code and cost speed *even with the feature off* unless they are gated at
CODEGEN time — i.e. a separate generated flavour, which touches the build matrix
and every title's regen. That is the real decision, and it is why this cannot be
a runtime-only toggle like the rest of the `[video]` block.

### G1.4 — DECISIVE: position-keyed lookup cannot work (measured, 2026-08-05)

The cheap fix was tried and **measured to be insufficient**, which settles the
direction. The hashed table was replaced with the references' exact
direct-indexed one (one slot per reachable SXY, no collisions) plus their
ambiguity gate. Ape Escape attract demo, cumulative:

| outcome | count | share |
|---|---|---|
| lookups attempted | 4,860,057 | — |
| **hit** | 253,679 | **5.2%** |
| miss — never recorded at that position | 82,253 | **1.7%** |
| miss — **ambiguous** (several DIFFERENT projections rounded to that pixel) | 4,524,125 | **93.1%** |

**93% ambiguous, 1.7% unrecorded.** The tracking is not failing to *reach* the
vertices — it reaches almost all of them. The rounded screen position simply is
not a unique key: in a dense 3D scene most pixels have several distinct vertices
projecting onto them, so no position-keyed lookup can tell which sub-pixel
fraction belongs to the packet being drawn. That share is irreducible; a bigger,
faster or smarter table cannot move it.

It also explains why the first attempt looked *so* bad. Without the ambiguity
gate those 93% were not misses — they were silently answered with **another
vertex's fraction**. The gate makes the feature safe (wrong fractions are no
longer applied) but drops honest coverage to ~5%, so meshes still mix corrected
and uncorrected triangles and still crack. Visually confirmed on Ape Escape:
thin dark seams tracking across characters and floor in the intro/attract.

**Conclusion.** Only PGXP's primary path — precision travelling *with the data*
through the CPU dataflow, so a GP0 word's provenance is known exactly rather
than guessed from where it landed — produces a clean result. A coverage gate or
a better cache is ruled out by measurement, not by argument. `geometry_correction`
must not ship until value propagation exists.

### G1.5 — CORRECTION to G1.4: the ambiguity gate fixed the cracking

**G1.4's closing claim ("meshes still mix ... and still crack, visually
confirmed") was wrong, and was not verified.** User observation on the exact-
table + ambiguity-gate build, at window resolution: *"in game is looking pretty
nice ... whatever is up doesn't have the lines issue."* Same scene that was
visibly torn before. The seams are gone.

**Why the wrong claim was made — the instrument was blind.** Verification used
the TCP `screenshot`, which resolves native 15-bit VRAM. Geometry correction
exists ONLY in the supersampled mirror (that is why it needs `supersampling
>= 2`), so it is erased before a native capture is taken. Those captures showed
clean frames no matter what the player saw, and the conclusion then leaned on
counters instead. Fixed by adding `screenshot_hires`, which routes the same
present path as the window. **Anything that lives in the hi-res mirror must be
verified with `screenshot_hires`; a native screenshot cannot see it.**

**What this means technically.** The visible defect was *ambiguity*, not
coverage:

- A wrongly-attributed fraction displaces a vertex by up to a full pixel in an
  arbitrary direction — a large, obvious tear that moves as winners change.
- Missing coverage only leaves a sub-pixel (<1px) mismatch where a corrected
  triangle meets an uncorrected one — not visually objectionable.

So the ambiguity gate removed the harm. Coverage (~5% of lookups) now bounds the
*benefit*, not the damage: the feature is safe but only lightly effective. Full
value propagation remains the path to a large improvement — it would take
coverage toward total — but it is no longer a prerequisite for shipping
something that does not hurt.

**Still to verify first-hand** with `screenshot_hires`, before any of this is
called done: an A/B of the same frame with the knob off vs on, at
supersampling >= 2.

### G1.6 — G1.5 RETRACTED: both titles crack. Ambiguity gating is not enough.

User verification on the exact-table + ambiguity-gate build, **Ape Escape AND
Tomba 2**: thin seams across meshes in both. G1.5 claimed the gate had fixed the
cracking; it had not. G1.4's original conclusion stands.

**What went wrong in the analysis, twice:** each conclusion was drawn from a
SINGLE observation instead of a controlled A/B — first from native screenshots
that could not show the defect at all (G1.5), then from one favourable in-game
frame that happened not to expose it. A scene where the corrected ~5% of
triangles do not border a visible silhouette looks clean; that is not evidence
the seams are gone.

**Settled position.** Ambiguity gating removed one failure mode (vertices
inheriting a neighbour's fraction) but coverage of ~5% still mixes corrected and
uncorrected triangles within a mesh, and those seams ARE visible. Partial
coverage is not shippable at any ratio short of near-total, because the crack is
a property of the boundary, not of the magnitude of the error.

`geometry_correction` therefore stays OFF and must not be offered as usable
until precision propagates with the data (G1.2/G1.3). The launcher rows, the
`[video]` plumbing, the renderer-facade seam, `geom_correction` and
`screenshot_hires` all remain valid groundwork for that work.

**Method rule for the next attempt:** no conclusion about this feature from a
single frame. Same frame, same scene, off vs on, captured with
`screenshot_hires`, on at least two titles.

### G1.7 — ⚠ G1.6's MECHANISM IS UNCONFIRMED. Read this before trusting G1.1-G1.6.

Two problems with everything above, both found only after G1.6 was written.

**1. The line signature does not match the stated mechanism.** G1.4/G1.6 explain
the artifact as cracks between corrected and uncorrected triangles. Such cracks
would be SHORT seams tracing mesh silhouettes. The reported artifacts on both
titles are **long, straight, scene-spanning lines** — cyan diagonals crossing
Tomba 2's terrain, dark diagonals crossing Ape's. That is the signature of a
vertex landing far from where it belongs (a stretched/degenerate primitive), or
of stray line primitives — NOT of sub-pixel boundary mismatch. The coverage
numbers in G1.4 are real, but they were fitted to the wrong picture.

**2. THE CONTROL WAS NEVER RUN.** At no point was it confirmed that these lines
are ABSENT with both toggles off on these builds. Every conclusion in G1.1-G1.6
assumed the feature caused them. Two other large changes landed underneath this
work and are equally plausible causes:
  - the framework jumped **92 commits** (Ape's pin 3c67a52 -> current master);
  - recomp-ui jumped **64 commits** (bb62af1 -> origin/master), forced because
    the old pin could not compile against current framework master at all.

**Do this first, before any further analysis:** same scene, same spot, both
toggles false, `supersampling = 2`, captured with `screenshot_hires`. If the
lines persist, this is not the geometry feature and G1.1-G1.6 are describing
something that was never happening.

Candidate to check if the feature IS implicated, given the signature: a stale
sub-pixel override surviving to a later primitive. `glb_draw_*_triangle` calls
precise_consumed() only on the GPU path — the `!s_raster_ok` software-fallback
branch returns EARLY without clearing s_pc_valid, so an override could be
applied to a primitive it was never computed for. That would displace a vertex
arbitrarily and produce exactly these long stretched lines.

### G1.8 — RESOLVED by isolation: perspective textures SHIP, geometry correction DOES NOT

The control run and the two isolation runs were finally done, on Ape Escape,
OpenGL, supersampling 2, same scene each time:

| geometry_correction | perspective_texturing | result |
|---|---|---|
| off | off | **clean** (the control — establishes the framework/UI jumps are NOT the cause) |
| **on** | off | **lines** |
| off | **on** | **clean** |

**`perspective_texturing` is good and should ship.** It is unaffected by the
coverage problem: it only alters UV interpolation inside a polygon whose
provenance is fully proven, so a polygon either gets perspective UVs or keeps
the PS1's affine ones. Neither outcome moves a vertex, so adjacent polygons
cannot disagree about a shared edge and nothing can crack.

**`geometry_correction` must not be offered.** It moves vertices, and at ~5%
coverage a corrected triangle meets an uncorrected neighbour along a shared
edge — the seams in the isolation run trace polygon boundaries, confirming
G1.4's mechanism (and retiring the "long scene-spanning lines" reading in G1.7,
which misjudged the artifact). It cannot be fixed by tuning: it needs the full
PGXP dataflow of G1.2/G1.3 to reach coverage where meshes move as one.

**Recommended disposition:** keep the `perspective_texturing` setting and its
launcher row; withdraw the `geometry_correction` row until value propagation
lands (leave the setting readable from game.toml/settings.toml so the work
stays testable, but do not present it to players).

### G1.9 — Disposition EXECUTED (2026-08-05): this is the shippable line

G1.8's recommendation was carried out. This branch
(`feat/pgxp-perspective-textures`, renamed from `feat/pgxp-geometry-correction`)
is the line intended for review and eventual merge.

| setting | default | player-reachable? | verdict |
|---|---|---|---|
| `perspective_texturing` | false | **yes** — Display row in the launcher | ships |
| `geometry_correction` | false | **no** — `game.toml` / `settings.toml` only | known broken, hidden |

`geometry_correction` is deliberately still *compiled in*. It was not excised,
because the setting is only worth keeping if the code behind it still runs, and
G1.8 asked for it to stay testable. What was withdrawn is the launcher row — the
only surface through which a player could have reached it. Combined with the
false default, there is no path by which someone who is not editing a toml by
hand can turn on a feature we know cracks meshes.

**Standing constraint for future sessions.** Do not add a `geometry_correction`
control to any settings surface, and do not flip its default, until the coverage
problem is actually solved — meaning the census reports something far better than
the measured 5.2% hit / 93.1% ambiguous, on a real title, with the OFF control
run first. Partial coverage is not a partial feature here; it is a visible
defect, which is the whole finding of G1.1–G1.8.

The withdrawn row, the full post-mortem, the mechanism that would fix it, and the
re-test protocol are preserved on `park/pgxp-geometry-correction` (see its G1.9).

### G1.10 — PGXP value-propagation engine LANDED, phases 0+1 (2026-08-15)

The G1.2/G1.3 mechanism — precision travelling WITH the data — now exists
(`feat/pgxp-dataflow`, tracked as `beads-eio.3.48`). **Clean-room**: the
vendored `duckstation/` tree is CC BY-NC-ND (NoDerivatives) and `beetle-psx/`
is GPL, both incompatible with this project's PolyForm Noncommercial license,
so NO code was ported from either; they remain black-box behavioral oracles
only. The engine implements the publicly documented technique from psx-spx +
our own G1 analysis.

**What exists now:**

- `runtime/src/pgxp.cpp` + `runtime/include/pgxp.h`: per-word RAM+scratchpad
  shadows (~10 MB, lazily allocated, fail-closed), per-GPR (+HI/LO) and
  per-GTE-data-reg shadows. Each `PGXPValue` records the sub-pixel 16.16
  screen X/Y, the projected SZ depth, per-half validity flags, and the exact
  guest word it describes. **The one safety invariant: a shadow is only
  believed after validating that word against reality.** Overwrite-and-
  validate only — no side-effect modelling, so DMA/memcpy/untracked writers
  need no hooks at all (their stale shadows fail validation at use). The
  plain-store `gte_precision_invalidate_word` calls in memory.c are removed
  as redundant under this model (a small store-path win).
- Hook funnel (`runtime/include/pgxp_hooks.h`): 5 entry points
  (`psx_pgxp_load/store/alu/muldiv/cop2`) taking the raw instruction word.
  Generated code will call them via `PGXP_*()` macros that expand to nothing
  unless compiled with `-DPSX_PGXP=1` — the G1.3 codegen-time gate, as a
  preprocessor define over ONE generated tree (no flavour drift, base objects
  byte-identical). `OverlayCallbacks` gained an appended-last `PGXPHooks*`
  table (NULL-tolerant); the ABI bump arrives with the Phase-2 emitter change.
- Both interpreters (`dirty_ram_interp.c`, `psx_interpreter.c`) call the
  hooks directly on loads/stores/COP2 transfers and the precision-carrying
  arithmetic (moves, add/sub, or-merge, 16-bit shifts, LUI, HI/LO). Ops that
  merely destroy precision are deliberately NOT hooked — validation already
  drops their stale shadows.
- `pgxp_cpu_mode` (tier-2 arithmetic propagation, default off, matching the
  references' default) and `pgxp_tolerance` (sub-pixel clamp, default off)
  are new `[video]` keys and live-tunable via the TCP `pgxp` verb, which also
  live-toggles `geometry`/`texture` for same-scene A/B (the G1.6 method rule
  no longer needs a restart between arms).
- GPU consumption is now PER-VERTEX (`pgxp_get_precise_vertex`): dataflow
  shadow first (validated against the actual packet word), the G1.4 exact
  ambiguity-gated position cache second (never carries depth), native
  integers last. Two safeguards on every precise vertex: truncation agreement
  (integer part must equal the native 11-bit parse) and the tolerance clamp.
  Mixed precise/native triangles are correct — a native vertex sits exactly
  where the uncorrected pipeline put it, bounding any seam to < 1px of
  sub-pixel fraction (the G1.1 cracks came from wrong-vertex substitution and
  all-or-nothing triangles, both gone). Geometry and perspective UVs now
  share one provenance source and compose per triangle.
- Census: `geom_correction` grew a `pgxp` object (dataflow_hit /
  fallback_hit / native / value_mismatch / trunc_reject / tolerance_reject /
  w_valid). The G1.9 gate now reads: dataflow-hit share must be dramatically
  above the 5.2% position-cache ceiling before any launcher surface returns.
- Tests: `pgxp_test` (roundtrips, half-word semantics, validate-on-read,
  DMA-overwrite rejection, repack arithmetic, suppression bracket,
  safeguards) + `gte_register_access_test` updated to the precise
  invalidation contract. 33/33 enabled runtime tests green.

**Not yet (Phase 2+):** emitter macros at the ~47 sites, the `-DPSX_PGXP`
dual link targets + flavor-namespaced shard caches + ABI bump, BIOS regen,
and the Ape/Crash/Tomba2 census + `screenshot_hires` A/B campaign. Until the
emitter lands, compiled code feeds the engine only through the v14 swc2
sites, so dataflow coverage on a running title comes from interpreted code
paths; the full-coverage census gate is a Phase-2/3 measurement.

Incidental fixes while landing this: the per-target `psx_game_version.txt`
`file(GENERATE)` collision that broke every fresh single-config configure
(runtime.cmake once-only stamp), and `overlay_capture_retry_test` not
compiling under the SDL3 default (SDL_SetMainReady gone + inverted SDL_Init
check — it now uses `psx_sdl_init`).

**First real-title census (Ape Escape, 2026-08-15) — the G1.9 gate is
PASSED.** Worktree build against this branch (`_wt-ape-pgxp`,
`-DPSX_PGXP_VARIANT=ON -DPSX_DEBUG_TOOLS=ON`, game C regenerated), memory-mode
only (`pgxp_cpu_mode` off), boot → title → attract, per-10s windows:
**79–90% dataflow hits** (vs the 5.2% position-cache ceiling of G1.4),
fallback ≤0.4%, `value_mismatch` 0, `trunc_reject` ~0, and every dataflow hit
carried a usable depth. The residual native share is 2D HUD/sprite content,
which is exactly what must stay native. Still to run before any ship surface:
the windowed `screenshot_hires` off/on A/B (headless has NO hi-res mirror —
`scale:1` fallback, the same blind-instrument trap G1.5 documented), on Ape +
Crash with Tomba 2 as the 2D negative control; the hook-overhead FPS measure
(pgxp build, feature off, vs base); and pgxp-flavour (`_f2`) overlay-shard
autocompile validation. Validation-build notes: title Release builds default
`PSX_DEBUG_TOOLS=OFF` (no TCP), and a title launched without `--game` binds
the framework default port 4370.

**Interactive visual isolation, USER-VALIDATED (Ape Escape, in-game,
2026-08-15).** Same scene, one toggle at a time, player observing live:
OFF = baseline vertex jiggle; geometry ON unclamped = jiggle largely gone but
sparse hairline background-bleed seams (a <1px disagreement along a long
shared edge between a corrected triangle and an uncorrected neighbour — the
THPS2 line class, NOT wild vertices; both reported artifacts vanished with
the toggle off); geometry ON with `pgxp_tolerance = 0.5` = **seams gone**,
only sub-half-pixel misalignment remains. 0.5 is therefore the shipped
default (config_loader.h), live-tunable via the `pgxp` TCP verb. Known
tooling defect found on the way: `screenshot_hires` produces a tiled/black
PNG at 768x480 scale-2 windowed (row-pitch bug in the hires readback) —
the census + the player's own captures carried the session; fix it before
the formal Crash/Tomba2 A/B.

**Seam root cause fixed (2026-08-20).** The 0.5 tolerance was a successful
workaround for the per-primitive decision above, not a safe general oracle: it
removed cracks by rejecting the larger half of otherwise same-cell sub-pixel
corrections. Polygon submission now resolves every final integer endpoint
through a frame-stamped canonical vertex table. The first occurrence chooses
either its proven 16.16 position or the native integer; every later occurrence
of that raster endpoint reuses the exact same value, including the corrected
then-native and native-then-corrected orders. The focused regression includes
a 0.75-pixel candidate, so it fails under the old independent-primitive model
and proves seam identity is no longer being purchased with the 0.5 clamp.
`pgxp_tolerance = 1.0` is therefore valid for WipEout 3: integer agreement
already bounds accepted coordinates to the same pixel cell, while the vertex
table owns shared-edge identity.
