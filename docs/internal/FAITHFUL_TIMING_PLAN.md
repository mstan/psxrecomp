# Faithful Timing Core — Game Plan (psxrecomp)

**READ THIS EACH SESSION.** Referenced from CLAUDE.md Rule -1 and from the
auto-memory ([[psxrecomp-build-faithful-core-not-hacks]],
[[precise_irq_slice_state]]). This is the authoritative plan; update the
"Status / Log" section every session.

---

## 0. North star + guardrails (non-negotiable)

Build the **faithful hardware-timing core** of the static recompiler. The PSX
recompiler is being BUILT, not preserved:

- The correct fix is ALWAYS the faithful, class-level core — NEVER a surgical
  per-game patch, symptom workaround, `game.toml` hack, or "make native agree
  with interp even if both are fake."
- Breaking other titles is acceptable; they were built on a faulty ecosystem and
  will be **regenerated**. Backward-compat is NOT a constraint.
- No stubs, no HLE, no interpreter-as-fallback (Architecture A locked). Fix the
  recompiler/runtime and regenerate; never edit `generated/*.c`.
- Don't guess (PRINCIPLES). Confirm every mechanism with the oracle + rings
  BEFORE changing code. Build observability first.
- Confer with ChatGPT via the **Chrome MCP browser at chatgpt.com** — the
  existing "PSX Static Recompiler Debug" chat (the user has Plus logged in).
  NOT the `codex` CLI (usage-limited).

## 1. The problem (diagnosis, confirmed)

A static recompiler charges cycles **block-granular** (instruction count up front
at each block leader) and checks IRQs at **block edges**. Real HW and the
sanctioned dirty-RAM interpreter have a **per-instruction** cycle timeline and
take IRQs at the exact instruction. Games that read timers / poll IRQ-driven
flags in tight loops fork between backends.

Tomba 2 (SCUS-94454) logo→FMV stall is the canonical case. The cascade:
1. Timer1 debounce value-fork @ pc 0x8008592C (frame 1823) — fixed by exact block
   cycle costs ("Fix A", already in tree).
2. **CURRENT BLOCKER:** measured **−8 cycle drift** (native BEHIND interp),
   entering in the BIOS→overlay init transition (func 0x80050B0C subtree). The
   frame-1824 logo-delay wait loop (caller 0x8008AE48 → RCnt reader 0x80085900;
   exits when *0x80102748[=960] < elapsed Timer1) loops ~1557× in interp (reaches
   FMV) vs ~42× native (stuck on logo).
   **Mechanism (located in code_generator.cpp):** when a branch's delay slot is
   ALSO a block leader (`exit_has_delay && !delay_slot_in_block`, ~line 1243), the
   branch block's `instruction_count` excludes the delay slot; on the TAKEN path
   the delay-slot clone runs but its cycle is charged by neither the branch block
   nor the (unentered) delay-slot block → undercount 1/site. ~8 sites = −8.
3. Interrupt take-point granularity (block-edge vs exact instruction) — the
   "precise IRQ slicing" track. PARKED (default off); it is a later correctness
   upgrade, NOT the FMV blocker. Validated design exists (block-leader
   continuations); see §5.

## 2. The target architecture (what "faithful core" means)

Per ChatGPT (validated) + standard practice:
- ONE shared **per-instruction cycle-cost function** `psx_instr_base_cycles(pc,
  insn)` used by BOTH the dirty-interp and the recompiler. No two approximate
  models.
- Recompiler emits **exact** accumulated cycle charges (collapses to a constant
  per pure-compute block); every dynamically executed instruction charged exactly
  once; delay slots owned by the branch bundle.
- **Segmented charge** before any guest-visible time observation (MMIO read/write
  to timers/GPUSTAT/SPUSTAT/DMA/CD/I_STAT/I_MASK, BIOS/device calls, backedges,
  calls/returns) so native and interp observe devices at the same architectural
  boundary.
- **Timers derived on-demand** from a global guest-cycle counter at read time;
  DMA/CD/GPU/IRQ on **scheduled event deadlines** (not per-cycle ticking) so
  compiled stays fast.
- Invariant: *every execution backend may differ in host implementation, but not
  in guest-visible time.* At same-PC convergence points, native cycle total ==
  interp cycle total.
- pc=0 means ONLY a real guest pc=0 / explicit termination — NEVER "dispatcher
  couldn't re-enter." Fail closed + log on undispatchable PCs.

## 3. Phased plan (each phase: confirm → build → regen → run → measure → screenshot)

- **P1 — Cycle-audit observability.** Add a per-function/at-convergence cycle
  audit: record native vs interp cumulative guest cycles at same-PC points; expose
  via TCP/ring. SUCCESS: reproduces the flat −8 and pinpoints the entering site(s).
- **P2 — Delay-slot cycle ownership (the −8).** Fix in code_generator.cpp: branch
  bundle charges its delay slot; not-taken fallthrough → branch_pc+8 (not the
  delay-slot leader); delay-slot-as-standalone-leader charges itself; no
  double-count on the not-taken path. SUCCESS: audit shows −8 → 0; Tomba 2 reaches
  the intro FMV (screenshot). Likely the FMV unblock.
- **P3 — Shared per-instruction cost function.** Single `psx_instr_base_cycles`
  consumed by both backends; recompiler emits exact accumulated charges with
  MMIO/boundary segmentation. SUCCESS: first-divergence hashes identical past frame
  1824 across a longer run; audit stays 0.
- **P4 — On-demand timers + event deadlines.** Timer1/2/0 computed from global
  cycle counter at read; devices on scheduled deadlines. SUCCESS: no perf
  regression; timing-sensitive paths stable.
- **P5 — Precise take-points (fold in parked work).** Re-enable slicing; emit
  EVERY block leader as a CPS continuation (global dispatch → owning func w/
  cpu->pc; never a new entry; fail-closed on undispatchable). SUCCESS: exact-
  instruction IRQ delivery with bounded (one-block) hand-back.
- **P6 — Regression + faithfulness.** Regen + screenshot-smoke ALL titles (BIOS,
  Tomba 1, MMX6, Ape, Tomba 2); delete Tomba2 `overlay_native_block` (must still
  reach FMV/title); calibrate the shared model against Beetle/psx-spx. Pin bump is
  user-gated.

## 3b. Cycle-cost model SOURCE (no clean-room needed — transcribe + verify)

The HW-intended cycle model is documented AND available as reference source we
already have in-tree (our oracle's own code). Stage-2 = transcribe the NUMBERS
(facts, not GPL-protected expression; also in psx-spx) into OUR shared cost
function, then VERIFY each against Beetle at runtime. Do NOT paste Beetle code
(architecture differs + GPLv2 hygiene); write our own informed by the facts.

Extraction map — `psxrecomp/beetle-psx/mednafen/psx/` (main checkout):
- **CPU base / instruction fetch:** cpu.cpp `ReadInstruction()` (~L534) — icache
  model: `timestamp += 4` cache-disabled (0xA000_0000+), `+3` on cache miss/fill,
  `+1` per fill word, near-0 on hit. For a static recompiler this becomes a
  per-block fetch-cost constant (assume cache-enabled steady state; calibrate).
- **Memory wait-states:** cpu.cpp `ReadMemory()` (~L365) / `WriteMemory()` (~L454)
  — `timestamp += (ReadFudge>>4)&2`, the `lts` delta from `PSX_MemRead*` is the
  region wait-state; `LDAbsorb = lts - timestamp` is the load-delay absorb. Charge
  in OUR psx_read/write path by region (RAM fast, BIOS ROM slow, scratchpad fast,
  MMIO per-device). Split clean from CPU base (don't double-count).
- **Mult/Div latency:** cpu.cpp `MULT_Tab24` (~L101), `muldiv_ts_done` (~L154) —
  mult/div set a completion timestamp; a later MFHI/MFLO stalls until then. Model
  as a documented latency (mult ~6-13 by operand magnitude via MULT_Tab; div/divu
  ~36). Encode as instruction cost + optional stall-on-read.
- **GTE/COP2 per-command cycles:** gte.cpp `GTE_Instruction()` (L1713) returns the
  count via each op fn (DPCS/MVMVA/NCDS/…). Well-known table (also psx-spx):
  RTPS=15 RTPT=23 MVMVA=8 SQR=5 OP=6 AVSZ3=5 AVSZ4=6 NCLIP=8 NCDS=19 NCDT=44
  NCCS=17 NCCT=39 NCS=14 NCT=30 CC=11 CCS? CDP=13 DPCS=8 DPCT=17 DCPL=8 INTPL=8
  GPF=5 GPL=5 (verify each against gte.cpp op-fn returns + psx-spx before use).
- **Timers (already partly faithful):** timer.cpp — divider ratios already used
  (T1 hblank ÷2146 etc.); move to on-demand counter = f(global cycles) at read.

Build order for the model (P3 → Stage-2):
1. Shared header (single source of truth) consumed by interp (runtime) AND
   recompiler (it already includes ../../runtime/include/*.h): identity first
   (cost=1) → regen → prove byte-identical generated cycle charges (zero behaviour
   change) → seam established.
2. Fill real costs from the extraction map, ONE component at a time, each verified
   against Beetle at runtime (native cumulative cycles == Beetle at convergence).
3. Memory wait-states in the psx_read/write path (region table).
DO each transcription with the Beetle source open + a runtime cross-check; a wrong
cycle number CREATES divergence, so verify, don't rush.

## 3c. STAGE 2 — full hardware cycle accuracy (the goal; -8 is DONE/past)

The -8 was backend-disagreement (native vs our interp); FIXED (FMV reached). Stage 2
makes the cycle model match REAL R3000A timing, validated against Beetle. We are NOT
hardware-cycle-accurate yet: model is ~1 cycle/instruction; Beetle charges ~2x.

### The validation breakthrough: DELTA comparison (offset-independent)
Absolute-cycle comparison through boot is meaningless (native is ~121M cycles off
Beetle due to turbo-loads/overlay load-model differences). BUT the cyc_watch
comparator's per-hit DELTAS cancel that offset: between two consecutive hits of the
same anchor (one iteration of identical code), native charged 46 cycles vs Beetle 91
(@0x80017FC4). That ~2x gap IS the cycle-model inaccuracy, measured cleanly. So:
  VALIDATE STAGE 2 BY MATCHING native Δcycles == Beetle Δcycles over identical
  regions (consecutive same-anchor hits, or entry/exit anchor pairs), NOT absolute.
First concrete target: make the 0x80017FC4 inter-hit Δ 46 -> 91 (== Beetle).

### Stage-2 progress log
- #1a data-load cost DONE (2ef47bd): psx_instr_base_cycles +2 per CPU load (LWC2 +1).
  Δ gate @0x80017FC4: native per-iter 46 -> 56 (Beetle 91). FMV still streams (no
  regression). Closed ~10/45. Approximation: no scratchpad-free / region / load-delay
  ABSORB yet — those are refinements (absorb would LOWER native, so it's not the
  remaining 35; the remaining gap is other components below).
- REMAINING ~35 cyc: DISASSEMBLED func_80017FC4 — it is only loads/stores/ALU/branches
  + a countdown delay loop; NO mult/div/GTE/MMIO. So the gap is NOT those, for this fn.
  BUT func_80017FC4 exits via a CPS TAIL-CALL to 0x8001EFFC (no normal return), so the
  single-anchor entry-to-next-entry window SPANS MULTIPLE functions (80017FC4 ->
  8001EFFC -> ... -> re-call). => single-anchor Δ is TOO COARSE for per-component
  attribution; the 56/91 covers code we haven't disassembled.
- TOOLING NEXT (before more cost components): add a TWO-ANCHOR region mode to cyc_watch
  (capture cycles at region START anchor A and END anchor B; report Δ(B−A) per pass) on
  BOTH backends. Then validate the cost model on a KNOWN, fully-disassembled single
  code path (no calls/loops crossing out) — e.g. a leaf function entry→its terminator.
  That gives rigorous per-component attribution instead of an opaque multi-fn window.
  Only then resume adding components (fetch / mult-div-stall / GTE / load-absorb).

### Components to transcribe (from in-tree Beetle + psx-spx; verify each by Δ)
The ~2x gap is dominated by what 1/insn ignores. Implement one at a time, re-measure Δ:
1. **Memory access wait-states (biggest lever).** Real loads/stores cost >1 cycle by
   region (RAM/BIOS-ROM/scratchpad/MMIO). Beetle: cpu.cpp ReadMemory `lts` delta +
   LDAbsorb (load-delay). Charge in the load/store path: interp exec_one's mem ops AND
   the recompiler-emitted cpu->read/write (or a per-load/store charge). Region table.
2. **Instruction fetch / I-cache timing.** Beetle ReadInstruction (+1 hit / +fill on
   miss). For the recompiler, fold a per-block fetch-cost constant.
3. **Mult/Div latency.** Beetle MULT_Tab/muldiv_ts_done: mult ~6-13, div ~36, stall on
   HI/LO read. Encode in psx_instr_base_cycles (+ optional stall-on-read).
4. **GTE/COP2 per-command.** Beetle gte.cpp GTE_Instruction table (RTPS=15, NCDS=19,
   NCDT=44, ...). Encode in psx_instr_base_cycles for COP2 ops.
All land in the single-source psx_instr_base_cycles (opcode costs) + a memory-path
wait-state charger (address-dependent). Both backends consume the same model (seam
already in place). Each component: transcribe -> regen/build -> Δ-compare vs Beetle
on a fixed region -> next.

### Caveats
- Δ-region must be IDENTICAL code on both (a tight loop body, or a pure-compute
  function). Avoid regions that cross turbo-load / overlay / dirty boundaries.
- Relocated BIOS-shell funcs (phys 0x30000-0x5AFFF) dispatch at a different native
  phys — anchor on game-text / BIOS-ROM, or the relocated phys.
- This is a multi-component effort; do it methodically, one validated component at a
  time. The comparator (cyc_watch + cycle_compare.py) is the validation backbone.

## 4. Tooling / oracle
- Runtime TCP port 4500; Beetle oracle 4382. Always-on rings: `event_ring`,
  `wtrace_all` (write trace; `newest=1`). `freeze_check` has slice-trace + cycle
  fields. `PSX_EXIT_HALT=1` halts-and-serves at the pc=0 exit for post-mortem.
- Build runtime: `cmake --build Tomba2Recomp/build-t2 --target psx-runtime`
  (PATH=/c/msys64/mingw64/bin). Recompiler: `cmake --build
  _wt-tomba2/psxrecomp/recompiler/build-t2 --target psxrecomp-game`. Regen:
  `recompiler/build-t2/psxrecomp-game.exe --config game.toml` (rebuild tool first).
- Reference: nocash psx-spx; the dirty-RAM interp is the in-process oracle for
  compiled code; Beetle is the HW oracle.

## 5. Status / Log (update every session)

- **2026-07-20 (netplay match_caps — host settings enforce):**
  Lobby `create` / `set_match_caps` / `start` carry host sim caps
  (aspect, turbo_loads, bios_hle, fast_boot, auto_skip_fmv, input_delay,
  language). Server echoes on join/update/launch; guests apply before boot.
  recomp-net-server + psx_lobby_client + MotK launcher. SNES mirror:
  widescreen/hud/ignore_aspect/input_delay/ws_extra via snes_lobby + MW main.

- **2026-07-20 (launcher: persist controller selection immediately):**
  Device/mode/deadzone now write `settings.toml` on change (not only Launch),
  so Refresh / Quit / soft-return keep the pad. Refresh falls back to saved
  GUID if the dropdown index is stale.

- **2026-07-20 (lobby default → public host):**
  `psx_lobby_default_url` now `ws://netplay.technicallycomputers.ca:8765`
  (match SNES); override still `PSX_NET_LOBBY_URL`. Synced MotK vendored
  `psx_lobby_client.{c,h}` + recomp-net `docs/lobby.md`.

- **2026-07-20 (Metal Warriors H2H: top-edge prop pop):**
  Full-frame present recenters dual cam ~$40 up; spawn/OAM top was only
  −$70/−$70 so platforms popped at Y=0. Spawn −$A8, OAM CMP −144, present
  Y wrap peek for −64..0, dist-limit +64 when vert-widen.

- **2026-07-20 (launcher: controller Refresh rescan):**
  Dashboard Device row (P1 + offline P2) has Refresh — pumps SDL joysticks,
  re-enumerates gamecontrollers, keeps selection by GUID. Offline + netplay.

- **2026-07-20 (launcher: lobby lock emoji via symbol fallback font):**
  Password lobbies showed □ for 🔒 — LatoLatin has no emoji. Load bundled
  `NotoSansSymbols2-Regular.ttf` (system Segoe UI Symbol on Windows) with
  RmlUi `fallback_face=true` so missing glyphs resolve.

- **2026-07-20 (launcher lobbies: button order + dblclick join):**
  Lobbies actions: Return to Launcher → Change Player Name → Host Game →
  Join Lobby. Double-click a lobby row joins (same path as Join Lobby,
  including password modal).

- **2026-07-20 (launcher: offline↔netplay switch buttons):**
  Dashboard footer: "Switch to Netplay" beside Launch Game (only when
  `PSX_HAS_RECOMP_NET`); "Switch to Offline" beside Netplay Lobbies.
  Home Netplay tile gated the same way; no-netplay builds skip home chooser.

- **2026-07-20 (netplay load — apply freeze after hash match):**
  Suppressing INPUT at `np_begin_load_apply` deadlocked both peers in
  `netplay_barrier_admit`: tips stopped → `try_admit` never succeeded →
  guest never ran → `savestate_poll` never applied. Fix: keep INPUT during
  APPLYING; suppress only at `np_enter_load_ready` until `hard_resync`+prime.

- **2026-07-20 (netplay load — false peer_disconnect → lobby):**
  Hash-match apply suppresses INPUT for seconds → `peer_disconnected(1500)`
  fired → soft-exit to lobby → rematch. Fix: timeout=0 (BYE-only) while
  `in_load_barrier`; HELLO keepalive every 250 ms during suppress/stall.

- **2026-07-20 (netplay post-load — stale INPUT clobber):**
  2nd+ loads: correct frame, then ~3–5s frozen while FPS lived. Cause: during
  LOAD apply/ready the slower peer kept emitting pre-resync INPUT tips; those
  ticks share ring slots with the new tip (`tick % 128`) and first-wins /
  overwrite races blocked `remotes_ready_for_sim` after `hard_resync`. Fix:
  suppress INPUT sends for the load barrier; reject out-of-window remote ticks;
  host `probe_finish` before sync+prime; re-anchor frame pacer on restore.

- **2026-07-20 (savestate load — force GL present after identical frame):**
  2nd+ load of the same `.pst` left FPS climbing while the picture stayed
  frozen: `gl_renderer_present_vram` / wide early-out skipped `SwapWindow`
  when display rect + present-dirty matched the last swap (common after
  restoring into an already-shown frame). Fix: `gl_renderer_invalidate_present`
  marks all present tiles dirty, clears path latches, resets interp history,
  and forces 8 presents; called from `psx_frontend_on_savestate_loaded`.

- **2026-07-20 (netplay post-load — admit barrier symmetric):**
  Host dropped `LOAD_READY` before `try_admit` (guest did not) → confirm
  wait with barrier already down; keeping remotes let stale tip=D
  first-wins. Fix: both stay in `LOAD_READY` until admit; `hard_resync`
  clears remotes again; sync+prime at mutual ready only.

- **2026-07-20 (netplay post-load — mutual-ready sync):**
  `hard_resync`+prime at apply let the later peer wipe the earlier tip
  (2nd load slower). Sync once at mutual ready. (Remote-keep reverted —
  see admit-barrier note above.)

- **2026-07-20 (netplay post-load — symmetric ready release):**
  Guest cleared `LOAD_READY` on READY ACK while host still had
  `state_stall_sim` → confirm wait / intermittent hitch. Guest now ACKs
  but stays in `LOAD_READY` until `try_admit` succeeds (pre-sends
  INPUT_CONFIRM so host can admit on the same poll as `probe_finish`).
  Ready-probe retransmit 40→8 ms; confirm retransmit 16→4 ms. MotK rebuild.

- **2026-07-20 (netplay post-load resume — frozen picture + FPS):**
  After load, FPS kept climbing while the window stayed on a stale/blank
  frame: (1) present blank-latch skipped redraw after restore; (2) delay
  rings empty after `hard_resync` while peers could still advance during
  APPLYING. Fix: `hard_resync` resets `sim_tick→0` + `prime_delay_inputs`;
  stall admit for APPLYING when `!savestate_pending` and all LOAD_READY;
  `psx_frontend_on_savestate_loaded` forces restage/blank once; skip FPS
  CLI during load barrier. MotK rebuild.

- **2026-07-20 (netplay host-only save/load commands):**
  User `savestate_request_*` refused on netplay guest; F-keys / debug TCP /
  `PSX_LOAD_SLOT` host-only or routed via `psx_netplay_request_*`. Guest
  follow-host sync uses `savestate_request_*_protocol`.

- **2026-07-20 (netplay load — post-restore lockstep rendezvous):**
  Hash-match load applied on each peer at different times after early
  `hard_resync` → rings/ticks drifted → admit hang / starvation. Fix:
  stage load → apply while admit runs → `hard_resync` only after restore →
  LOAD size=0 ready probe until both ACK → then resume. Heartbeat in
  admit barrier. MotK rebuild.

- **2026-07-20 (netplay save hang — coord probe must not stall):**
  Shift+F1 stalled admit before `savestate_poll` could write → deadlock.
  Fix: `STATE_PROBE` with `size==0` (coord) leaves admit running; only
  hash probe (`size!=0`) + chunk transfer stall. Guest retransmit replies
  without re-staging saves. MotK rebuild after sync.

- **2026-07-20 (netplay host-owned saves — hash probe + chunk transfer):**
  recomp-net: `RNET_STATE_MAX` → 8 MiB, `STATE_PROBE`/`PROBE_REPLY`, stall
  admit through probe + transfer; restored `rnet_session_wait_recv`.
  psx_netplay: guest sandbox `saves/netplay/`, host-only F-keys, match-start
  memcard probe, save = coord local write → hash-agree → transfer on miss +
  post-CRC verify; load same pattern + `hard_resync`. MotK `build-release`
  linked; verify Shift+F1 / F1 across LAN.

- **2026-07-20 (MotK title after FMV — leave-depth24 restage):**
  On exit from GP1 depth24, GL/VK `depth24_upload_policy` restaged full
  CPU VRAM as 1555 into the FBO. CPU still held packed RGB888 from MDEC →
  rainbow/static title background (text/overlays still drew as prims).
  V2: skip only framebuffer-sized depth24 transfers (keep texture A0s);
  on leave scissor-clear the skipped FB union (GL) — never blind-restage
  RGB888-as-1555. Char-select shrink-to-left-center still under probe
  (OFX=256 @ 512 CRTC looks correct; may be authored layout / separate).

- **2026-07-20 (MotK 2nd intro right-edge stretch):**
  `depth24_fix_trailing_margin` replicated the last good column when any
  chroma>40 pixel sat in the trailing 8 cols. On the starfield FMV that
  smeared stars into an 8-wide flickering strip. Now requires dense chroma
  (~12% of margin) and black-fills instead of column-replicate. Still no
  CRTC/content_w crop.

- **2026-07-20 (MotK netplay FMV — lockstep floor, not present path):**
  A/B: offline headless FMV ~59; two offline headless concurrent ~59; two
  netplay headless ~38–40. Not dual-CPU contention and not GL present.
  Lockstep (INPUT_CONFIRM frame barrier + same-tick input rendezvous)
  keeps both MDEC peaks aligned. Async confirm / peer drift made FMV
  *worse* (~22–28) via overlapped memory traffic. Barrier now UDP `poll()`
  (not `SDL_Delay(1)`); localhost peers pin to disjoint CPU halves (~45
  in A/B). Pipeline admit rewrite hung tick-0 — reverted.

- **2026-07-20 (MotK netplay FMV — restore pre-rematch vblank order):**
  User: same-machine netplay was 50–60 before rematch playback tweaks.
  Reverted `NetplayVblankGuard` present-before-admit and half-rate depth24
  present. Order is again finish→admit→pace→present. Kept: `s_present_w/h`
  clear (black rematch FMV), depth24 FBO upload skip, trailing-margin
  in-buffer fix, vsync-off while lockstep armed.

- **2026-07-20 (MotK netplay tick-0 hang — revert admit latency hacks):**
  After half-rate present, both peers armed lockstep then sat at frame 0
  until peer_disconnect. Cause: `SDL_Delay(0)` busy-spin starved peer UDP
  on dual localhost; same-call `try_admit` publish when CONFIRM pre-seen
  also unsafe. Reverted both.

- **2026-07-20 (MotK FMV netplay FPS — half-rate depth24 present):**
  Measured: headless dual-peer lockstep holds ~60 guest FPS through intro;
  windowed dual-peer ~30–40. Bottleneck is two GL CPU-presents serializing
  before the guest fiber resumes — not admit/guest. Fix: under netplay +
  depth24, present every other vblank (host-only; admit still every tick).

- **2026-07-20 (MotK FMV netplay FPS — skip depth24 FBO upload queue):**
  MDEC A0 was still queued as 1555 CPU→FBO uploads (`UP_RECTS_MAX`=16),
  force-flushing mid-movie. While `gpu_display_is_depth24()`, do not queue
  GL/VK uploads; on leave, drop queue (no full restage — see leave-depth24
  log above). Alone did not restore
  windowed netplay to offline rates (present cost remained).

- **2026-07-20 (netplay FMV host FPS — present/vsync ordering):**
  Offline MotK intro ~50+; netplay ~30–40 was host path, not guest
  divergence. Fixes (determinism unchanged): (1) force GL/VK swap
  interval 0 while lockstep is armed (restore on soft-exit) so driver
  vsync does not double-block after the wall pacer; (2) move
  `finish_frame`→present→`admit`+pacer so local Swap overlaps the peer's
  guest quantum. `turbo_loads` stays off in netplay.

- **2026-07-19 (MotK FMV right-edge — no present-width crop):**
  Upload-span + `content_w` left-aligned GL crop removed the chroma junk
  but replaced it with a flickering black pillar (span varied per frame,
  especially during lighting). Rework: keep full CRTC width always;
  `depth24_fix_trailing_margin` only replicates the last good column
  into the last 8 RGB cols when chroma junk is detected — in-buffer,
  no viewport shrink. Half-texel nearest UV clamp remains.

- **2026-07-19 (MotK FMV right-edge — trailing margin, not CRTC shrink):**
  Root cause: MotK depth24 crawl is 512×128 CRTC, but ~8 trailing RGB
  columns are stale/black in VRAM; GL edge sampling flickered that strip
  as garbage. Fix (no 2/3 width): track A0 upload span
  (`gpu_depth24_rgb_limit`); blank/crop last 8 cols on short depth24
  bands (`h<240`); `gl_renderer_present(..., content_w)` left-aligned
  UV crop + half-texel nearest clamp; screenshot skips `sync_cpu` on
  depth24 (was clobbering RGB888). MotK release+PGO rebuilt; user-verify
  2nd intro (Star Wars logo) edge + ~50 FPS.

- **2026-07-19 (MotK FMV right-edge / 2/3 revert):**
  Tried depth24 width=(CRTC*2)/3 (512→341) for right-edge junk; MotK
  intros rendered left-shifted with the right of the video clipped —
  confirms the Jul-18 finding (logo is centered in a 512 RGB line).
  Reverted 2/3. Kept: depth24→fmv_frame, short-band without force_4_3
  gate, nearest present on depth24. Right-edge junk needs a different
  fix (not shrinking CRTC width).

- **2026-07-19 (MotK FMV FPS after rematch patches):**
  Rematch video fixed; ~30–40 vs prior ~50+ was not a present-path
  regression. Prior ~50 med was MotK intro PGO (`PSX_PGO=use`); LTO-only
  baseline is ~39. Mistakenly cleared PGO — restored `PSX_PGO=use` with
  existing intro `.gcda`. `fmv_frame` restored (depth24 only forces
  `pin_43` short-band letterbox). Re-train via `scripts/pgo_motk_intro.sh`
  after large rematch edits if profiles go stale.

- **2026-07-19 (netplay rematch: black FMV = stale present tex size):**
  Root cause: `s_present_w/h` survived GL context destroy; rematch FMV same
  size as prior CPU present took `glTexSubImage2D` into a new unallocated
  `s_present_tex` → black. Cleared on shutdown + init_context. Also dropped
  per-frame `flush_cpu_uploads` on depth24 (was cutting intro ~50→~30 FPS);
  depth24 CPU scanout needs neither FBO sync nor upload flush.

- **2026-07-19 (netplay rematch: black FMV, audio OK):**
  Rematch linked and played, but MotK intros had XA audio with black video
  (FPS still dipped ~30–40 → MDEC ran). Earlier mis-attribution to sync_cpu;
  also reset present/FPS/MDEC/iso session statics; `iso_close` before reopen;
  depth24 forces 4:3 pin for short-band letterbox.

- **2026-07-19 (netplay rematch: sticky I_STAT/I_MASK):**
  After cycle-reset fix, rematch still starved: dump meta showed
  `psx_cycle_count=4` with leftover `i_stat=VBlank` + game `i_mask`.
  `interrupts_init` / `memory_init` now clear I_STAT/I_MASK (+ mem_ctrl);
  `starvation_ring_reset` on `session_reboot` so dumps are rematch-clean.

- **2026-07-19 (stick→D-Pad axial deadzone again):**
  Radial+sign for digital stick→D-Pad made left/right fire Up/Down from tiny
  Y drift (jump/crouch). Stick→button sources use per-axis `controller_deadzone`
  again; analog `axes_to_pad_pair` / hybrid stick-detect stay radial.

- **2026-07-19 (netplay rematch: stuck `psx_in_device_service`):**
  Rematch still froze after `lockstep armed`: soft-exit longjmps out of the
  vblank callback while inside `psx_devices_service_to_now`, leaving
  `psx_in_device_service=1`. Every later `psx_advance_cycles` then skips
  device service → no vblanks → hang after tick-0. Fix: clear the guard on
  every scheduler longjmp escape; `psx_cycles_reset_for_boot()` zeros the
  guest clock + deadline bookkeeping at `session_reboot`.

- **2026-07-19 (Digital stick→D-Pad radial deadzone):**
  Stick-as-D-Pad used a per-axis square threshold so centre drift twitched
  movement even with a large launcher deadzone. Stick axis→button sources now
  require radial magnitude past `controller_deadzone` (same idea as
  `axes_to_pad_pair`); triggers stay per-axis. Hybrid stick-detect matches.

- **2026-07-19 (netplay rematch HLE shell-skip latch):**
  After soft-return rematch both peers linked (`lockstep armed`) then froze:
  `s_shell_skipped` stayed set from the first match so HLE boot-skip never
  re-fired and both ran the interactive BIOS shell under netplay.
  `psx_bios_hle_configure` now clears the latch; `cdrom_init` resets boot
  disc speed to 1x; lobby launch uses `input_player=-1` (auto) again.

- **2026-07-19 (netplay rematch session_id + endpoint guard):**
  Rematch HELLO hang: server now allocates a fresh `session_id` on every
  `start`/`launch` (stale BYE/HELLO from the prior UDP session no longer match)
  and refuses start when host/guest endpoints are empty. Client refuses
  `fill_netplay_launch` / `launch_pending` when `peer_hostport` is missing.

- **2026-07-19 (netplay return-to-lobby rematch):**
  Lobby WS stays up across Launch. Window-close / Escape / peer BYE soft-exits
  via `PSX_RUN_RETURN_TO_LOBBY` (scheduler longjmp or pre-entry flag) instead of
  `exit(0)` when the match started from a lobby room. Teardown keeps the WS;
  launcher resumes on `netplay_room` with ready cleared; rematch Launch
  re-enters `session_reboot` (re-init guest + netplay). Server clears ready on
  `start` so both peers must Ready again.

- **2026-07-19 (lobby server → closed-source Rust):**
  Proprietary `recomp-net-server` (Rust) owns WS lobby + privacy/docs;
  removed C `servers/lobby` from open `recomp-net`. Client WS helpers
  vendored at `runtime/src/lobby_ws/`. Default
  `ws://netplay.technicallycomputers.ca:8765`.

- **2026-07-19 (netplay lobby server + launcher menus):**
  Lobby WS+JSON owned by proprietary `recomp-net-server` (was C
  `servers/lobby/` under open recomp-net);
  `psx_lobby_client` + RmlUi home → Offline / Netplay → lobbies table
  (host/join/password). Launch hands `PsxNetplayConfig` to
  `psx_netplay_start` (LAN endpoints from lobby). ICE relay stubbed.

- **2026-07-19 (netplay peer disconnect QoL):**
  Barrier `SDL_PollEvent` on `SDL_QUIT`/Escape → `shutdown_runtime`+exit.
  recomp-net `BYE` (pkt 7) + `rnet_session_peer_disconnected(~1.5s)`;
  `psx_netplay_shutdown` sends BYE. Surviving peer prints and exits instead
  of spinning in admit.

- **2026-07-19 (netplay latch + INPUT_CONFIRM + exclusive capture):**
  Host stages one pad per sim tick (`latched_for_tick`); barrier only
  re-samples via `needs_local_sample`, stalls on `input_desync`
  (INPUT_CONFIRM hash mismatch). Netplay capture is exclusive to the
  assigned PlayerInput (no keyboard-all / all-controllers merge) so peer
  hashes agree. Cleared on `finish_frame` advance. recomp-net: preserve
  early peer INPUT_CONFIRM when activating (wipe raced slower peer into
  permanent stall). Smoke: two headless MotK peers both `lockstep armed`,
  frames advance, no INPUT desync.

- **2026-07-19 (netplay lockstep stall + per-peer input device):**
  True delay-sync gate: pre-scheduler + each vblank `finish_frame` then
  blocking `poll_admit` (guest fiber parks; no free-run on admit fail /
  linking). Auto/`--net-input-player`: host samples P1 device, guest samples
  P2 when assigned (same-PC C40+keyboard); pad blob deadzone normalize.

- **2026-07-19 (netplay pad ownership — session slots always plugged):**
  Host local device → net-slot 0 (sim P1); guest local → net-slot 1 (sim P2).
  While active, SIO is network-only (no local/`override` writes). Both session
  ports stay connected from `psx_netplay_start` through linking so in-game
  2P/VS detect works; `refresh_player_devices` no longer clears them.

- **2026-07-19 (delay-sync netplay bring-up — recomp-net LAN):**
  Wired `recomp-net` into the runtime as CLI/env LAN delay-sync (not GGPO).
  `psx_netplay.{c,h}` + CMake auto-discover `../recomp-net`; vblank owns
  `pump`/`try_admit`/`publish`/`advance`; local pads stage only — publish is
  sole SIO writer while active. Turbo + low-latency re-sample gated off.
  Lobby UI / ICE server deferred. Smoke: two procs with `--netplay --net-slot`.

- **2026-07-19 (MotK title/char-select — savestate PC + flat GEO batch):**
  User still saw ~10 FPS after draw-area reject. Real char-select profile:
  ~30k/s on-screen GP0(68h) starfield dots, `gpu_share`~0.8 (not empty clip).
  Also: `boot_state_load` forced `pc=entry_pc`, so F1 loads desynced (display
  off; false ~60 FPS). Restored saved PC; batched flat GEO tris in
  `gpu_gl_renderer.c`. Char-select Shift+F1: **~60 FPS locked**, gpu_share~0.06.

- **2026-07-19 (MotK title/char-select — GPU draw-area reject re-applied):**
  Shift+F1 title + Arcade char select were &lt;10 FPS: same OT drain as the
  inter-movie cliff — GP0(E3/E4)=(0,0)-(0,0) + thousands of clipped `0x68`
  dots / quads; GL built 2 tris/prim. Re-applied inclusive draw-area reject
  in `gpu.c` (prior revert blamed a “gap race”; crawl wrap was the separate
  24bpp present-width bug). Alone insufficient for live starfield menus.

- **2026-07-19 (MotK intro — native/hot/inline + multi-run PGO):**
  Shipped `-march=native`, `[recompiler] hot_funcs` for VLC
  `0x8006A9F8`/`0x8006CBE4`, Release-inline `debug_server_log_call_entry`
  (cpu_state.h), HIT-inline `psx_icache_fetch` (psx_icache.h), multi-run
  PGO train (`PGO_TRAIN_RUNS`/`PGO_TRAIN_SECS`). Remeasure clean logo still
  **~51 med** (more mid/high-50 samples; not locked 60). Load-delay cycle
  volume remains the ceiling. Inter-movie cliff open. No MotK VSync HLE.

- **2026-07-19 (MotK intro — PGO + advance_cycles host cost):**
  (1) `psx_advance_cycles`: drop per-charge watchdog/PC-sample (moved into
  `service_to_now`, HARD_CAP cadence); (2) `psx_devices_mmio_sync` recomputes
  deadline in-place instead of dirtying `next_service=0` (was forcing service
  on the next insn after every GPU/CD/MDEC MMIO); (3) MotK intro PGO via
  `scripts/pgo_motk_intro.sh` (`-DPSX_PGO=generate|use`, 311 .gcda). Clean
  logo (until inter-movie cliff): **~49–51 med** (was ~39 LTO-only). Crawl
  after gap recovers ~47–50. Inter-movie ~7 FPS GPU cliff still open. No
  MotK VSync HLE.

- **2026-07-19 (MotK intro — VLC host opts + Release LTO):**
  Host-side work on MotK VLC (`0x8006A9F8`/`0x8006CBE4`), not disc cache:
  (1) idle_skip no longer defeats IRQ fast-path; deferred idle GPR snap;
  (2) IRQ mid-path when bits already pending; (3) `psx_slice_block` header
  inline when parked; (4) main-RAM `psx_cyc_load_word`/`half` inlined;
  (5) MotK Release `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE` (-flto).
  Logo window: ~34 med → ~39 med (samples mostly 38–40). Still short of 50;
  load-delay cycle volume remains the ceiling (PSX_LOAD_DELAY=0 → hundreds
  FPS). Inter-movie ~5–7 FPS GPU cliff open. No MotK VSync HLE.

- **2026-07-18 (MotK intro — FMV pace: inline cycle advance):**
  Host FPS still short of 50 with real MDEC/XA. Inlined `psx_advance_cycles`
  (deadline fast path) + merged load fudge/cost into one charge; bit-exact
  DC-only MDEC IDCT. Release FMV ~33 med / ~39 avg (steadier; was dipping to
  teens). Gap ~7. Remaining: static VLC `0x8006A9F8`/`0x8006CBE4` + load-delay
  host cost. Do not revive MotK VSync HLE / HARD_CAP without mdec rising.

- **2026-07-18 (MotK intro — 24bpp CRTC width + short-band present):**
  MotK FMV CRTC is 512 (X1/X2÷5); logo centered in 512 RGB (~86..431).
  Reverted blanket 24-bit mode×2/3 (341 cropped the right). Width = GP1(06h)÷
  dot-clock for 15/24-bit. Short GP1(07h)=128 no longer fills 4:3 — present
  letterboxes src_h/240 (GL/SDL/VK). Inter-movie GPU cliff still open.
  CD-only `spu_render` kept.

- **2026-07-18 (MotK intro FMV — false 60 FPS / no video; rollback):**
  MotK `load_accel.vsync_query` + event-horizon_any / HARD_CAP→564480 /
  in-exception VBlank chunking produced host FPS ~60 with **no MDEC**
  (`mdec_decode_count` stayed 0, display disabled) — guest time raced past the
  STR. A/B: VSync(-1) HLE alone is enough to keep MotK MDEC at 0; disabling
  it restores decode/XA. Reverted those accelerations for MotK; kept sticky
  CD IRQ deadline, load-delay coalesce, MDEC DMA bulk/IDCT skips. Real intro
  with video is again ~30–40 FPS host-bound under load-delay. Do not claim
  MotK FMV pace wins without `fmv_state.mdec_decode_count` rising.

- **2026-07-18 (earlier same day — sticky CD deadline + load-path host cost):**
  `cdrom_cycles_to_irq` sticky presented IRQ → 1-cycle deadline after sync;
  fixed. Load-delay coalesce + inline `psx_advance_cycles`. Necessary but not
  sufficient for MotK FMV ≥50 with video.

- **2026-07-11 (Tomba 2 OpenGL full-attract performance + audio acceptance):**
  Resolved the shared renderer/overlay/capture cascade that made Beach, Whoopee
  FMVs, Mines, and Mine Cart slow. OpenGL now avoids mandatory present readback,
  batches Tomba's painter-ordered blend stream, and suppresses unchanged 30 Hz
  source presents. Overlay dispatch now distinguishes exact lazy entries from
  CPS interior continuations: continuations use the loaded range owner first,
  while exact entries reached inside local dirty flow can publish their cached
  native DLL. The hot `0x80106424`/`0x80106688` FMV helpers consequently dropped
  from ~60 interpreted entries/frame to zero. Small dynamic-text DLL images are
  mapped on a bounded worker (141 Tomba variants, not the 712-DLL vault), and
  auto-capture base64/file I/O now runs from a coherent RAM/seed snapshot on a
  low-priority worker. The audio bridge uses real callback duration, bounded
  P-only correction, and a measured 160 ms reserve inside its existing 250 ms
  ring. Release acceptance: 540 s / 107 five-second records, Beach -> Whoopee ->
  Mines -> Mine Cart -> repeat, guest min 59.76 Hz / median 59.94 Hz, **zero
  output underruns**, zero post-start overflows, and no cache growth (849 DLLs).
  Current-code screenshots visually confirmed Beach, FMV, Mines, and Mine Cart.

- **2026-07-10 (Tomba 2 Whoopee auto-skip dwell + native-wide Beach backdrop):**
  Added an opt-in silent-MDEC post-decode hold so presentation-side FMV
  auto-skip remains unpaced through a preloaded logo's authored release wait;
  the faithful guest timeline is unchanged and the default remains four
  vblanks. Added title-opted native-wide mirror gates for flat primitives and
  the textured pre-shaded backdrop phase. This keeps the canonical 4:3 buffer
  untouched while filling Tomba 2 Beach Town at 16:9 and 21:9 without stretching
  the later 3D foreground. Also fixed JSON escaping in the `fmv_state` path and
  extended its resolved-config reporting. Validated Release build, unattended
  first-attract captures at 4:3/16:9/21:9, and zero unknown dispatches.

- **2026-07-02 (HLE PIVOT implemented — HLE as a first-class swappable tier, gbarecomp model):**
  USER-DIRECTED pivot (supersedes "no HLE" §0; CLAUDE.md amended 2026-07-02, memory
  hle_tier_architecture.md). Built the full stack this session: (1) EMITTER —
  full_function_emitter.cpp now emits a null-by-default `g_psx_bios_hle_hook` consult at
  the top of every psx_dispatch_impl iteration (pre-normalize phys, BEFORE the game/
  dirty-RAM/static backends; handled ⇒ resume at $ra; NULL default = pure LLE,
  dispatch-identical). (2) RUNTIME TIER — runtime/src/bios_hle.c(+.h): v1 call-HLE =
  the B0 event family (DeliverEvent/OpenEvent/CloseEvent/TestEvent/EnableEvent/
  DisableEvent) ground-truthed against the SCPH1001 kernel disassembly (Ghidra,
  0xBFC11644..0xBFC11A84; EvCB [0x120]/[0x124], stride 0x1C), operating on the real
  guest EvCBs, callback delivery via psx_dispatch_call with the kernel-true $ra
  0x1720; everything else (WaitEvent/threads/pads/card/A0/C0) falls through to LLE.
  (3) BOOT HLE — one-shot shell-entry intercept (RAM 0x30000; LoadRunShell's indirect
  call always dispatches): real recompiled kernel init + SYSTEM.CNF + EXE load run
  authentically under boot-turbo; only the shell (boot animation) is skipped. The old
  fast_boot snapshot restore is REMOVED (fast_boot=true now aliases boot-skip only).
  (4) SELECTION — [runtime] bios_hle / bios_hle_keep_intro / hle_scheduler in
  config_loader (+ settings.toml bios_hle mirror), PSX_BIOS_HLE / PSX_BIOS_HLE_KEEP_INTRO
  env, startup banner bios_backend=/bios_boot=. PSX_HLE_SCHEDULER spike folded in
  (default via psx_hle_scheduler_set_default, env wins). (5) OBSERVABILITY — always-on
  16K HLE ring (route LLE/HLE/boot-skip) + hle_dump TCP command (TCP_COMMANDS.md).
  Regen era-consistent: BIOS + Tomba + MMX6 images (emitter changed). NEXT: Tomba
  save+load validation under BOTH backends (user drives); overlay-shard cg-tag refresh
  per title; grow the handler set (UnDeliverEvent, RCnt, A0 libc) with kernel-decompile
  + Beetle checks per handler.
- **2026-07-01 (Tomba pause-menu wedge RESOLVED — stale-recompiler shards, guard shipped):**
  The post-merge Tomba menu wedge (loaded saves only) was NOT the IRQ-resume class the
  prior handoff claimed. Added an always-on exception-EXIT half to irqctx_ring
  (interrupts.c: take_pc/real_epc/exit_pc/exit_reason/same_thread/restored/v1/ra/redirects)
  — it proved every VBLANK resume restores the interrupted GPRs correctly, and the
  0x80016588 "spin" is the normal per-frame vsync wait. Real root cause: autocompiled
  overlay shards were emitted by a recompiler binary (build-t2, 07:39) OLDER than the
  09:28 emitter changes (553d993), yet stamped with the CURRENT cg tag (compile_overlays
  derives the tag from --runtime-include, nothing verified the emitting binary). The
  stale-emitter shards corrupted the display-list task queue (0x801FD800 slots) during
  load-game at Stormy Mountain → menu drew nothing. Repro matrix (compiled/interp ×
  newgame/load), cache-absent A/B, and RAM diff (empty OT 0x8009CA10 vs linked prims
  @0x800Bxxxx) pinned it; recompiler mtime vs emitter commit time was the smoking gun.
  FIXED: rebuilt recompiler, purged + regenerated build-cosim AND build-prod caches —
  menu opens with cache fully enabled. GUARD (class-closing, general): psxrecomp-game
  now bakes the emitter-source hash (shared canonical list
  runtime/codegen_hash_sources.cmake + hash_codegen.cmake) and prints it via
  `--codegen-hash`; compile_overlays.py HARD-FAILS when the binary hash ≠ the tag hash
  (verified positive + negative, both cache and --static modes). Kept the
  same_thread_resume GPR-restore refinement in interrupts.c (ring-verified equal-value
  no-op in practice; faithful). Tooling gaps logged in memory
  (divergence_tooling_gaps_2026_07_01). PENDING: MMX6 cutscene→gameplay gate
  (interrupts.c changed), user validation of prod build (menu + title/save-menu lag —
  lag likely the same stale-shard all-interp fallback + rehash churn).

- **2026-06-27 (device-region MMIO read waits — DONE, branch wt/tomba2-mmio-waits off the
  I-cache tip):** Replaced the placeholder `region = (phys<RAM_SIZE)?3:0` in psx_cyc_readmem
  (memory.c) with the full Beetle MemRW device-region read-wait table (libretro.cpp:859-1131),
  size-aware: main RAM (phys<0x800000) +3; SPU 0x1F801C00-1FFF +36 (32-bit) / +16 (8/16-bit);
  CDC 0x1F801800-180F +6×size; GPU/MDEC/SysControl/FrontIO/SIO/IRQ/DMA/Timers (within
  0x1F801000-113F) +1; BIOS ROM / Expansion-PIO / unmatched +0; scratchpad +0 (early-out).
  Threaded the access size (1/2/4) from psx_cyc_load_word/half/byte + psx_cyc_lwc2_read into
  psx_cyc_readmem (the SPU/CDC waits are width-dependent). The device wait combines with the
  existing +2 completion (+1 LWC2) and fudge exactly as Beetle ReadMemory (LDAbsorb = region +
  completion). RUNTIME-ONLY — psx_cyc_load_* signatures unchanged, so NO emitter regen; just
  rebuild runtime/cyctest. New ruler #2 loops `mmio_timer` (Timer0 read → +3 = 1 dev + 2 compl)
  and `mmio_spu` (32-bit SPU read → +38 = 36 + 2). VALIDATED: cyctest COMPILED (4600) == Beetle
  (4382) EXACT on ALL 15 loops incl. mmio_timer +3 / mmio_spu +38; the 13 prior loops unchanged.
  Tomba 2 boots past the BIOS to its "SCEA Presents" intro splash, total_checks advancing, no
  freeze (the faster-MMIO change did not trigger a device-timing cascade like load=4 did).
  RESIDUAL (documented, unmodeled dynamic axis): DMACycleSteal — Beetle adds the live DMA
  bus-steal count to EVERY read (libretro.cpp:868); non-zero only during active DMA, needs the
  steal count threaded out of the DMA controller, can't be isolated by a static ruler.
  memory.c + gen_testrom.py.

- **2026-06-27 (I-cache fetch — Stage 2 DONE: compiled-path emit + production default-on):**
  Both static emitters now charge the I-cache fetch cost at each cache-line LEADER, BEFORE
  the per-instruction interlock/load (Beetle ReadInstruction order, so a fetch miss clears
  the pending load give-back first). Leader = a block leader / mid-block jump-table target
  (any non-fall-through entry → possibly-cold line) OR a 16-byte-line start (addr&0xC==0);
  intra-line fall-through followers are provably hits (the leader refilled the line to its
  end) so they emit nothing (+0). code_generator (game): `addr` is already the KSEG0 runtime
  PC. full_function_emitter (BIOS): the loop addr is the ROM/compile addr, mapped to the
  RUNTIME guest PC via the existing `relocate_ra` (BIOS main stays in-place KSEG1 0xBFC..
  uncached; kernel Part 2 → 0x500+, shell → 0x80030000+) so the shared TV array evolves
  identically to the interp's cpu->pc and the KSEG1 (>=0xA0000000) uncached test sees the
  true virtual address. The compiled path emits at the SAME address value the interp would
  for the same instruction (they share s_icache_tv), so mixed compiled/interp stays
  consistent. VALIDATED: ruler #2 COMPILED (port 4600, PSX_ICACHE=1) == Beetle (4382) EXACT
  on all 13 loops incl. `icache_miss +14` (was +0 pre-Stage-2); ruler #1 [0x1C5C→0x1CA4]
  steady delta 0 (56==56) AND native now produces the I-cache cold-refill spikes (77/84,
  Beetle's range) that were absent before — exact per-hit magnitude varies run-to-run
  because the two processes free-run (Rule 16), not a bug. Tomba 2 boots past the load wedge
  → intro FMV (jungle) + crisp PS BIOS logo, total_checks advancing, no freeze. Then flipped
  `psx_icache_enabled()` DEFAULT ON (PSX_ICACHE=0 still disables for A/B) — re-validated the
  default-on path (cyctest no-env == +14; Tomba 2 boots clean). psx_icache.c + both emitters.
  NEXT axis: DMA cycle-steal / device-region MMIO load waits (SPU +36 etc.), currently
  unmodeled. Eventually merge wt/tomba2-load-accuracy to master after cross-title regen+smoke.

- **2026-06-27 (I-cache fetch — MODEL built + interp-validated EXACT; Stage 1 of 2):**
  New runtime/src/psx_icache.c: faithful direct-mapped (4 KB / 256-line) instruction-cache
  fetch cost, transcribed from Beetle PS_CPU::ReadInstruction — HIT +0 (no give-back clear),
  KSEG1/uncached +4, cached miss +3 + refill from the missing word to the line end (earlier
  words stay invalid), miss clears the load give-back. Mirrors only the per-word TV tag array.
  Wired into the dirty-RAM interp (exec_one) per instruction, charged BEFORE §1 (Beetle order).
  New ruler #2 loop `icache_miss` (loop top + victim 0x1000 apart alias the same line → refill
  miss every iteration). VALIDATED interp vs Beetle (PSX_FORCE_INTERP=1 PSX_ICACHE=1):
  icache_miss native +14 == Beetle +14; the other 12 loops unchanged at +0 fetch. So the
  hit AND refill-miss costs are MEASURED equal to the oracle on the interp path. Opt-in via
  PSX_ICACHE=1 (default OFF) — charging fetch in only one backend would fork mixed
  compiled/interp timing. Default-off → no production change (Tomba 2 FMV/logo verified
  byte-identical). Commit 958a928.
  STAGE 2 (pending): emit psx_icache_fetch at each cache-line leader in BOTH static emitters
  (code_generator + full_function_emitter), using the RUNTIME address (handle ROM->RAM
  relocated shell code); flip PSX_ICACHE default on so both backends charge it; validate
  ruler #1's cold first-hit spike (Beetle 84/77 vs steady 56) reproduces on the compiled
  path. RISK: the compiled cache state must match Beetle cycle-for-cycle across the whole
  boot to reproduce the cold spike — a careful cache-state-fidelity validation.

- **2026-06-27 (interp-path Δ-ruler — INTERP == Beetle EXACT on all 12 components):**
  Closed the last validation gap: the dirty-RAM INTERPRETER is now MEASURED equal to the
  oracle, not just shared-by-construction. New tooling: `PSX_FORCE_INTERP=1` makes
  `dirty_ram_is_dirty` (memory.c) report all RAM above the kernel window dirty, so the
  dispatcher routes clean compiled game text through the dirty-RAM interpreter (the same path
  overlays take) — no emitter/dispatch change. Launch psx-cyctest with the env set; the test
  ROM runs interpreted (dirty_ram_insns → hundreds of millions). measure.py --port 4600
  (interp) vs 4382 (Beetle): ALL 12 loops match EXACTLY (baseline/alu/load/load2/load_use/
  div/div_spaced/mult/gte_rtps/gte_nclip/gte_read_use/ld_div). Commit b0391bc.
  TWO PROCESS LESSONS (cost real time — now in cyctest README): (1) launch psx-cyctest via
  PowerShell Start-Process — a bash '&' launch fails to boot (pc=0); (2) sample cyc_watch /
  freeze_check AT STEADY STATE — an early query (before the BIOS boots to the EXE entry)
  reports dirty_ram_insns=0 / warm-up values. That premature-sampling artifact was the entire
  "dispatch paradox" I chased (the interp engages only after the EXE entry is reached).
  NEXT axis: I-cache fetch (ruler #1 84/77 cold-refill spikes).

- **2026-06-27 (GTE-read + MFC0 + muldiv give-back — IMPLEMENTED + VALIDATED, ruler #2 100%):**
  Closed the last steady-state divergences. KEY METHODOLOGY FINDING (Rule 15): Beetle's
  cyc_watch must be sampled at STEADY STATE — its boot/warm-up window reports the
  no-give-back value, which is why earlier sessions mis-recorded gte_rtps as "+15 EXACT"
  (true steady = +11). Added ruler #2 probes (load_use, gte_read_use, ld_div) to Δ-gate it.
  Shipped: `psx_gte_read` (MFC2/CFC2: stall to gte_ts_done AND arm ld_absorb=stall/
  ld_which_t=rt give-back; MTC2/CTC2 keep stall-only `psx_gte_stall`); MFC0 arms
  ld_absorb=0/ld_which_t=rt (suppresses a following load's fudge); `psx_muldiv_stall` now
  CONSUMES read_absorb during the MFLO/MFHI stall + the muldiv_ts_done-1 off-by-one — all
  transcribed from Beetle cpu.cpp:1332-1341/1723-1736, in both emitters + the interp.
  **All 12 ruler #2 loops == Beetle at steady state** (gte_rtps 18→14, gte_nclip 11→7,
  gte_read_use 19→14, ld_div 45→49 fixed; the 8 CPU-load/alu/div/mult loops held); ruler #1
  delta 0; Tomba 2 FMV plays (no regression). The R3000A load-delay + GTE/muldiv interlock
  is now hardware-faithful across every micro-benchmark. NEXT axis: I-cache fetch (ruler #1
  84/77 cold-refill spikes). Commits on wt/tomba2-load-accuracy (unpushed).

- **2026-06-27 (load ReadFudge/LDAbsorb — IMPLEMENTED + VALIDATED, both rulers exact):**
  Shipped the shared per-instruction R3000A load-delay interlock. New `runtime/include/
  psx_cyc.h`: §1 base + GPR_DEPRES + DO_LDS (`psx_cyc_step`) as static-inline helpers over
  new CPUState fields `read_absorb[33]/read_absorb_which/read_fudge/ld_which_t/ld_absorb`;
  `psx_cyc_load_word/half/byte` + `psx_cyc_lwc2_read` in memory.c do the Beetle ReadMemory
  timing (clear give-back, +2 fudge iff predecessor committed no load, region RAM +3 +
  completion +2/+1 as the LDAbsorb give-back, scratchpad +0). The pure dep/res classifier
  `psx_cyc_dep_res_mask` (transcribed from Beetle per-opcode GPR_DEP/RES) lives in
  psx_instr_cost.h. Wired into the dirty interp + BOTH static emitters (code_generator game,
  full_function_emitter+strict_translator BIOS); loads now route value reads through the
  UNCHARGED psx_read_* (cpu->read_* rewired in main.cpp; the flat +4 charge_main_ram_read is
  gone). **Δ-validated against Beetle:** ruler #2 `load2` +10 → **+11** == Beetle, every other
  component still exact (alu+1/load+5/div+38/mult+15/gte_rtps+15/gte_nclip+8); ruler #1
  [c5c→ca4] 54 → **56** == Beetle steady-state (84/77 spikes = I-cache cold refill, P2). Tomba2
  boots to the intro FMV (screenshot pixels, no regression). Builds clean (tools/BIOS/game/
  runtime/cyctest). FOLLOW-UP (separate commit): GTE-read/MFC0 give-back + muldiv-stall
  give-back consumption (don't affect rulers; needed for mixed-code faithfulness). Supersedes
  the "MODEL NAILED, impl pending" entry below.

- **2026-06-27 (load ReadFudge/LDAbsorb — MODEL NAILED empirically, impl pending):**
  Derived the last load-path component (the residual on both rulers) by measuring
  Beetle's PER-INSTRUCTION cost via adjacent-PC region cyc_watch. Confirmed:
  fudge = +2 iff the previous instruction committed no pending load (ReadFudge=0x20;
  `(reg>>4)&2` is 0 for all real regs), else 0; region+completion=5 (LDAbsorb excludes
  fudge); the load-delay-slot instruction does NOT absorb (its §1 precedes its DO_LDS),
  the instructions after it do. Per-instruction Beetle data: load = lw7/addiu1/bne0/nop0;
  load2 = lw7/lw6/addiu1/bne0/nop0. Full model + implementation spec in
  `accuracy/load_readfudge_ldabsorb.md`. Implementation is pervasive (per-instruction
  ReadAbsorb + GPR_DEP/RES in both emitters + interp) → a focused next task on a clean
  tree; validate via the ruler-loop anchors (baseline 3 / load 8 / load2 14). No code
  changed this step (read-only empirical derivation).

- **2026-06-27 (interp-path cycle ruler — enabler DONE + validated):** The dirty
  interp now emits `debug_server_cyc_observe(pc)` per instruction (gated), so
  interp-executed PCs are cyc_watch-anchorable (were not). Validated: a live
  Tomba2 overlay interp loop (0x8010724C) records stable cyc_watch hits at 38
  cyc/iter, parity with compiled-PC anchoring; FMV no-regression over 150M+
  interp insns. Commit fc85d8b. This lets interp-side cycle work (the muldiv +
  GTE stalls) be MEASURED, not just by-construction. REMAINING for a fully
  isolated single-component interp ruler: the cyctest harness does not route
  indirect jumps to the dirty interp (a jalr to a scratch dirty address left
  dirty_ram_insns=0 there), so a clean dirty-RAM component loop needs cyctest
  interp-dispatch wiring (or an overlay_cache=off Tomba2 component anchor).

- **2026-06-27 (GTE per-command completion-stall — VALIDATED EXACT):** Modeled
  GTE (COP2) command latency + stall-on-COP2-access. New CPUState.gte_ts_done;
  a GTE command arms it (now + cost-1, serializing back-to-back ops); any COP2
  reg access (MFC2/CFC2/MTC2/CTC2/LWC2/SWC2) stalls to it. Cost table
  (psx_cycles.c) transcribed+verified from beetle gte.cpp op returns (note
  AVSZ4=5 not the psx-spx-doc's 6). Set armed in the shared gte_execute (both
  backends); stall emitted at every COP2 reg-access site in both emitters + the
  interp (offset cancels like muldiv). Added gte_rtps/gte_nclip loops to ruler
  #2: native +15/+8 == Beetle +15/+8 EXACT; all other components unchanged;
  Tomba2 boots to FMV no-regression. Commit ec1fd76. Required regen. UNPUSHED.

- **2026-06-27 (dirty-interp mult/div completion-stall — backend parity):**
  Completed the mult/div stall to the SECOND backend. The dirty-RAM interpreter
  (Tomba2 overlays) charged 0 for mult/div while the compiled emitters already
  set `muldiv_ts_done` + stall MFHI/MFLO — an inter-backend cost inconsistency
  that drifts the shared guest-cycle timeline. `dirty_ram_interp.c` now mirrors
  the compiled emitter exactly via the shared helpers (MULT→`psx_mult_latency_s`,
  MULTU→`_u`, DIV/DIVU→37, MFHI/MFLO→`psx_muldiv_stall`), under
  `PSX_ENABLE_BLOCK_CYCLES`. The interp charges base after exec_one (vs compiled
  "+1 at top") but the set/stall offset cancels (verified algebraically). The
  latency VALUES are already oracle-EXACT on rulers #1/#2 (compiled path); this
  makes the interp apply the identical model. Validated: Tomba2 boots to FMV,
  no regression. Caveat: validation is by-construction + no-regression, NOT a
  direct interp Δ — interp emits no cyc_observe and the testrom isn't an overlay,
  so a true interp-path ruler (interp cyc_observe + force-interp routing) is
  future tooling. Commit 75d5d1a, runtime-only, no regen, UNPUSHED.

- **2026-06-27 (load=4 boot wedge RESOLVED — faithful guest-cycle pad ACK):**
  The oracle-accurate load wait-state (=4) had deterministically wedged Tomba 2
  boot in the BIOS shell (handle s1=104 → 1672-stride table index → wild ptr
  0x8013B608 → RAM corruption → pc=0). Step A (confirm, not hypothesise): the
  proximate runaway was **100% controller (pad) polling** — `sio_irq_dump` showed
  the last 150+ SIO IRQs all `source=pad, delay=4, active_device=PAD, mc_state=0`
  (card idle), NOT the memcard enumeration the handoff guessed. Source-confirmed
  unfaithfulness: the pad fast-path (`sio.c:1386`) armed the access-paced
  `sio_irq_countdown=SIO_IRQ_DELAY_PAD(4)`, decremented once **per SIO register
  access** (sio_tick is only ever called cycles=0), so pad ACK→IRQ7 was
  access-count-paced, not guest-cycle-paced; the faster (accurate) CPU fired it at
  the wrong guest-cycle phase vs the cycle-paced timers/VBLANK → BIOS pad-detect
  state machine diverged. Step B (faithful fix): the pad fast-path now arms the
  **guest-cycle-paced ack scheduler** (`sio_pending_ack`/`sio_ack_remaining =
  BAUD+ACK = 1258 cyc`, driven by `sio_advance`←`psx_advance_cycles`), identical
  to the already-faithful card path. RESULT: load=4 boots **past the wedge to the
  intro FMV** (screenshot-verified, frame 11k+ stable). Ruler #1 native 54 vs
  Beetle 56 = the known load-ReadFudge gap on the load=4 branch, NOT a regression
  (SIO timing can't change CPU instruction cost). Runtime-only (`runtime/src/sio.c`),
  no regen, UNCOMMITTED. Write-up: WEDGE_load4_shell_rootcause.md. Follow-up
  (completeness, non-blocking): axis5 Fix-6 / "1.0e-e2" fully removes the pad
  fast-path so pad+card share one shifter path — needs menu input validation.

- **2026-06-27 (BIOS emitter muldiv stall — ruler #1 now EXACT):** Applied
  per-instruction cycle charging + the mult/div completion-stall to the BIOS
  emitter (full_function_emitter.cpp: +1 at the top of every in-function
  instruction + the 4 inlined orphaned-delay-slot sites; block-up-front charge
  off in per-insn mode) and StrictTranslator (MULTU/DIV/DIVU → psx_muldiv_set,
  MFHI/MFLO → psx_muldiv_stall). RESULT: ruler #1 [0x80001C5C→0x80001CA4] native
  30→56 == Beetle 56, STEADY DELTA 0 — EXACT. Both rulers now match the oracle for
  mult/div (ruler #2 game-side already exact). FMV no regression (i_stat 0x8D,
  full frame). Commit 180b821. The +1-at-top convention cancels the divu-set /
  mflo-stall offset identically to the game emitter (both exact). NEXT: I-cache
  fetch (ruler #1 residual = Beetle's 84-on-cold-hit refill spikes vs native flat
  56); then load wait-state calibration (memory.c +6 → ReadFudge model); then GTE.

- **2026-06-27 (RULER #2 closed + mult/div completion-stall VALIDATED EXACT):**
  Built the full cycle micro-benchmark harness (ruler #2) and used it to land the
  biggest Stage-2 component.
  - **ruler #2 = `tools/cycle_testrom/`**: hand-encoded PS-X EXE of single-component
    isolation loops (baseline/alu/load/load2/div/div_spaced/mult), each measured by
    consecutive-anchor Δ = one iteration; baseline subtraction isolates the cost.
    Both backends boot the SAME synthetic disc (mkpsxiso; license region extracted
    from an OWNED disc via dumpsxiso — LOCAL ONLY, gitignored). Beetle loads it via
    --disc; native via a dedicated psx-cyctest runtime target (boots disc, serial
    CYCT-00101 so disc-identity matches). measure.py compares per-component costs.
  - **Beetle ORACLE costs** (the HW targets): baseline 3, alu +1, load +5, load2
    +11 (2nd load +1 = ReadFudge), div +38 (~36 stall), div_spaced +38 (fillers
    ABSORBED), mult +15 (~13 stall).
  - **MULT/DIV completion-stall IMPLEMENTED + VALIDATED EXACT.** MULT/MULTU/DIV/DIVU
    set CPUState.muldiv_ts_done = now+latency (DIV=37; MULT via MULT_Tab24 14/10/7
    on operand magnitude); MFLO/MFHI stall guest cycles to the deadline
    (psx_muldiv_set/stall in psx_cycles.c). Native previously charged ZERO. Required
    PER-INSTRUCTION cycle charging (PSX_CODEGEN_CYCLE_PER_INSN) — now the DEFAULT on
    this audit branch — so the stall absorbs (the running cycle count must be
    accurate mid-block; block-up-front can't). Game emitter emits set/stall at the
    op sites. RESULT vs oracle: div +38==+38, div_spaced +38==+38 (absorb correct),
    mult +15==+15 — ALL EXACT. Tomba 2 still reaches the FMV (no regression).
  - **Load double-count fix** (earlier today): psx_instr_base_cycles reverted to
    pure execute base (loads=1); memory.c owns the data-access wait-state.
  - Commits 9cec60a, 2b5ad88, 47bcfec, a3e8f28 (+ cyc_watch dedupe). NOT pushed.
  NEXT: (1) calibrate memory.c load wait-state (native +7 vs Beetle +5: flat +6 →
  ~4 + a ReadFudge term). (2) Apply per-instruction mode + muldiv stall to the BIOS
  emitter (full_function_emitter.cpp) + dirty interp → closes ruler #1's div-stall
  gap (still 30 vs 56). (3) GTE per-command cycles (same stall mechanism, gte.cpp
  table). (4) I-cache fetch. Each Δ-gated on the rulers, FMV-verified.

- **2026-06-26 (RULER #1 BUILT + load double-count bug found & fixed):** Built the
  game-independent BIOS-kernel cycle ruler the §3c "TOOLING NEXT" called for, and
  it immediately paid off. Details:
  - **New oracle-model doc `CYCLE_MODEL_BEETLE.md`** — transcribed the full R3000A
    cycle model verbatim from in-tree Beetle cpu.cpp (base +1/insn minus load-delay
    absorb; I-cache fetch +0 hit / +4 KSEG1 / +3+refill miss; ReadMemory loads
    scratchpad=0/region-wait+2, posted stores; mult 6-13 / div 36 stall-on-MFHI/LO;
    GTE per-command table). This is the calibration ground truth.
  - **cyc_watch double-fire FIXED** (debug_server.c): observe was called from BOTH
    the dispatcher (trace_dispatch) AND the function prologue (log_call_entry) at the
    same cycle → every dispatched entry double-recorded. Added (phys,cycle) dedupe.
    Real tooling bug (Rule 15); native deltas were corrupted before this.
  - **Per-block-leader cycle observe ADDED** (full_function_emitter.cpp →
    debug_server_cyc_observe, #ifndef PSX_NO_DEBUG_TOOLS so prod = zero overhead).
    Native previously observed only at FUNCTION ENTRIES; now it samples at EVERY
    compiled block leader, matching Beetle's before-every-instruction sample. This
    lets cyc_watch anchor ANY block-leader PC (interior loop tops, prologue exits) →
    a clean KNOWN-instruction region on both backends. Emitted at normalize_address()
    (runtime phys) so relocated-kernel anchors match.
  - **THE RULER:** BIOS kernel EvCB-search fn at guest 0x80001C5C (relocated ROM,
    identical in every PSX title). Region [0x80001C5C→0x80001CA4] (18-insn prologue,
    contains divu+mflo + 2 RAM loads, no MMIO/GTE). Both backends now record it.
  - **BUG FOUND — loads double-counted.** Native [c5c→ca4] = 34, fully decomposed:
    block c5c advance 20 (=14×1 + **2 loads×3**) + block c9c 2 + **memory.c +6×2 = 12**
    = 34. The Stage-2 #1a commit (2ef47bd) made psx_instr_base_cycles return 3 for
    loads (+2 data-access) WHILE memory.c's charge_main_ram_read already charged +6
    per main-RAM read — the load data-access cost was counted TWICE. The opaque
    0x80017FC4 window hid this because the load over-charge masked the entirely-
    unmodeled divu→mflo stall (~30 cyc Beetle, 0 native). **FIX:** reverted
    psx_instr_base_cycles to pure execute base (loads=1), per the header's own stated
    "data access charged separately in the memory path" contract. memory.c is the
    single address-keyed owner (like Beetle's ReadMemory). Regen BIOS+game, rebuild.
  - **RESULT:** native [c5c→ca4] 34→**30** (exact: 16+2+12), dead stable. Beetle 56
    steady (84 cold = I-cache line refill). FMV still streams (i_stat 0x8D, no
    regression). The remaining −26 gap is now HONEST and decomposed: native is
    missing the divu→mflo execute stall, and memory.c's flat +6/load needs Beetle
    calibration. NEXT: isolate those two components — the BIOS prologue combines
    div+loads in one block (leader anchors can't split them), so the principled next
    step is **ruler #2 (HW test ROM, Amidog)** for hand-crafted single-component
    isolation loops (div-only, load-only), per the user's "do both rulers /
    completeness not convenience" directive. Then Δ-gate each EXECUTE-latency
    component (mult/div, GTE) into psx_instr_base_cycles and CALIBRATE memory.c's
    wait-state per region. All on wt/tomba2-cycle-audit, uncommitted.

- **2026-06-26 (measure: Beetle cycle clock BUILT + VALIDATED):** Added absolute
  guest-cycle exposure to the Beetle oracle (MAIN checkout, additive diagnostic):
  beetle-psx/libretro.cpp accumulates per-frame `timestamp` (CPU->Run slice) into
  `beetle_total_guest_cycles` (+ reset on init) with `extern "C"
  beetle_core_get_guest_cycles()`; runtime/src/beetle_debug_server.c h_ping now
  reports `guest_cycles`. Rebuilt beetle static lib + psx-beetle. VALIDATED (Rule
  0): guest_cycles advances ~565,022 cyc/frame = real PSX rate (33.8688MHz/~59.94).
  Beetle needs the .CUE (not raw .bin). FIRST CROSS-CHECK: native psx_cycle_count
  rate = 565,470 cyc/frame vs Beetle 565,022 (within 0.08%) => gross cycle-rate
  parity confirmed; remaining drift is fine per-instruction-path (needs same-PC
  alignment). Main-checkout Beetle edits are UNCOMMITTED (additive; master has
  other prior uncommitted work — leave for user to manage).
  NEXT (aligned comparator): Beetle has get_registers (PC) but NO run-to/step/pause,
  so same-PC cycle comparison needs a "capture guest_cycles when guest reaches PC X"
  hook on BOTH servers (native has run_to_frame/step; Beetle needs a PC-watch). Then
  diff cycles@PC native vs Beetle to see the residual drift, and Stage-2 cost
  transcription verified against it.
- **2026-06-26 (P3 step 1 DONE — single-source cost seam, identity):** Created
  runtime/include/psx_instr_cost.h `psx_instr_base_cycles(insn)` (identity, 1/insn).
  Routed BOTH backends through it: interp (exec_delay_slot, dirty-dispatch loop,
  precise-slice) + recompiler (code_generator.cpp sums it per block + outside
  delay-slot clone, folding into the compile-time block charge). PROVEN behavior-
  preserving: regen byte-identical (full.c + dispatch.c diff = empty) and Tomba 2
  still streams the FMV (i_stat 0x8D). Commit b00b81f. Stage-2 now edits ONLY this
  one function. ACCURACY_BURNDOWN.md added (all-axes burndown; axis-5 peripherals,
  esp. SIO/controller hybrid-pad bug, flagged weakest), 09a5d45.
  NEXT (measure before Stage-2 costs — don't guess): build the native↔Beetle cycle
  comparator. Feasibility CONFIRMED: beetle_debug_server.c (in worktree) already
  exposes beetle_get_frame_count via the beetle glue — add a parallel
  beetle_get_guest_cycles. Sub-steps: (1) find mednafen's running master-cycle
  timestamp in beetle-psx/mednafen/psx (psx.cpp PSX_Update / the CPU
  pscpu_timestamp_t accumulator — note it's slice-relative, must accumulate to an
  absolute guest-cycle count); (2) add a C accessor through beetle_libretro.cpp +
  a `guest_cycles` debug command; (3) rebuild Beetle static lib + psx-beetle
  (slow: `cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1
  HAVE_LIGHTREC=0 -j8`); (4) comparator: native psx_cycle_count (already in
  freeze_check) vs Beetle guest_cycles at same-PC convergence. THEN transcribe
  Stage-2 costs (mult/div, GTE table, mem wait-states) one at a time, each verified
  by this comparator. Native cycle side already exists; Beetle side is the gap.
- **2026-06-26 (holistic cycle-model audit, post-P2):** Audited ALL cycle-charging
  sites for the dominant class (delay-slot undercount) + cost-model consistency:
  - GAME + OVERLAY emitter (code_generator.cpp `translate_basic_block`): FIXED in
    P2 (block_exec_cycles +1 for outside delay-slot clone). Overlay/alias path
    shares translate_basic_block → covered.
  - BIOS emitter (full_function_emitter.cpp): NO undercount — different model. It
    emits delay slots IN-LINE at their real address (charged by the owning block
    via block_cycles count to next leader) and defers the branch via
    psx_taken_/psx_delay_ flags, rather than emitting an uncounted clone. So the
    delay-slot-is-leader case charges correctly. No change needed.
  - INTERP (exec_one callers): charges psx_advance_cycles(1u) per instruction
    (3 sites). 
  => The cost MODEL is "1 cycle/instruction", duplicated in 3 places (interp hard
  1u; game emitter instruction_count; BIOS emitter leader-to-leader count). They
  agree (Stage-1 backend-equivalent) but are NOT a shared function and NOT HW-
  accurate (Stage-2). recompiler CAN include runtime headers (already includes
  ../../runtime/include/ws_backdrop_detect.h) → a shared psx_instr_base_cycles()
  header is feasible for P3.
  NEXT CORRECTNESS STEPS (deliberate, not tail-of-session):
  1. MEASURE FIRST (don't guess HW costs): native exposes psx_cycle_count already;
     build the Beetle half — add additive guest-cycle exposure to the Beetle oracle
     (main checkout beetle_debug_server.c) + a native-vs-Beetle cycle/first-
     divergence harness (find_divergence.py is STALE/DuckStation-era port 4371 —
     replace with a Beetle 4382 comparator). This is the holistic correctness
     instrument; it makes drift visible for ALL code/titles, FMV being one measure.
  2. P3 shared psx_instr_base_cycles() seam (identity first → byte-identical regen
     proof → then Stage-2 real R3000A costs calibrated against the measure).
  3. P6: regress other titles (breaking is OK per Rule -1; just know), delete
     Tomba2 overlay_native_block.
  COMMIT: f9d50d7 on wt/tomba2 (local, not pushed, not merged to master).
- **2026-06-26 (P2 DONE — Tomba 2 reaches the intro FMV):** Implemented the
  delay-slot cycle-ownership fix in code_generator.cpp (translate_basic_block):
  `block_exec_cycles = instruction_count + (exit branch sits AT end_addr with a
  delay slot outside the block ? 1 : 0)` — charges the always-executed delay-slot
  clone that was previously uncounted (the -8 undercount). Applied to BOTH the
  slice budget and the block cycle charge. Regen + build clean. RESULT: native
  progresses past the frame-1824 logo-delay loop; screen animates; i_stat shows
  CDROM+DMA+SIO active; screenshot = the lush jungle intro FMV. The multi-week
  logo stall is GONE via the faithful fix (no hack, overlay_native_block untouched
  for now). Mechanism was structurally confirmed (instruction_count excludes the
  outside delay-slot clone) before the change; end-to-end confirmed by FMV
  screenshot. NEXT: P3 (shared per-instruction cost fn + MMIO segmentation), P6
  validation (delete overlay_native_block; regress other titles), and Stage-2 HW
  cycle calibration vs Beetle/psx-spx (current model is 1 cycle/insn = backend-
  equivalent, NOT yet hardware-accurate). Apply the same delay-slot fix to the
  BIOS emitter (full_function_emitter.cpp) and overlay/alias paths.
- **2026-06-26 (earlier):** Diagnosis corrected (timing-faithfulness, not take-point).
  Directive persisted (CLAUDE.md Rule -1, memory, MEMORY.md banner). Precise
  slicing root-caused (mid-function clean-text resume not dispatchable) + ChatGPT-
  validated fix (all block leaders = CPS continuations) — PARKED default-off;
  `psx_game_is_function_entry` predicate + slice-trace diagnostics + env toggle
  `PSX_PRECISE_SLICE` left in tree (inert). −8 mechanism located in
  code_generator.cpp (delay-slot-is-leader undercount). Tree builds + boots clean.
  NEXT: P1 (cycle-audit) → P2 (delay-slot ownership fix).
