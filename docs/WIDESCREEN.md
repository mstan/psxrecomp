# Widescreen support (`feat/widescreen`)

Status as of 2026-06-13. Branch `feat/widescreen` in **both** `psxrecomp`
(framework) and `TombaRecomp` (game opt-in + config), pushed to remote.
Experimental. Latest: the far-backdrop edge void is largely fixed (2D backdrop
screenX squash + a depth-gated GTE-3D un-squash); a native-wide rewrite that
would retire the squash entirely was spiked and shelved (see below).

The framework feature is **generic and parameterized**: every game gets the
plain behaviour for free, all knobs default to inert, and `aspect_ratio = "4:3"`
(the default) is a mathematical identity — byte-for-byte the original
presentation. A game opts in via `[video] aspect_ratio` plus an optional
`[widescreen]` block. One build serves both aspects; the launcher's
**Settings → Aspect ratio** toggle selects it at runtime (persisted to
`settings.toml`).

This is the DuckStation/Beetle "widescreen hack" (squash the GTE projection,
present stretched to the wide aspect → wider field of view), but implemented
in our GTE library + GPU so it covers generated code, the interpreter, and
overlay DLLs uniformly — and extended well past what emulators ship with
(per-prim proportion correction, so sprites/HUD are NOT stretched).

---

## What works (committed, user-validated)

| Commit | What |
|---|---|
| `f8a29e8` | `[video] aspect_ratio = "W:H"` → GTE X-squash (`gte_set_display_aspect`, `runtime/src/gte.cpp`) + aspect-aware letterbox in both renderers + window opens at aspect. Config plumbed through `game.toml`, `settings.toml` (load+save), launcher seed (`recompiler/src/config_loader.{h,cpp}`, `runtime/src/main.cpp`, `runtime/src/gpu_gl_renderer.c`). |
| `970670b` | Launcher **Aspect ratio** setting: 4:3 (Native) / 16:9 (Widescreen) / 21:9 (Ultrawide). One build does all. |
| `cad8c96` | **Per-prim proportion correction.** Recompiler emits `psx_ws_sprite_tag(cpu)` at the entry of each `[widescreen] sprite_tag_funcs` function; runtime records prim ptr (`$a0`) → GTE-projected anchor and re-squashes tagged prims around their own anchor at GP0 execution, so characters/billboards are NOT stretched. Untagged textured rects (SPRT = screen-space HUD/menus) center-squash. (`recompiler/src/code_generator.cpp`, `runtime/src/gpu.c`) |
| `0792086` | **FMV 4:3 pillarbox** (24-bit OR streamed 15-bit MDEC video, detected via `mdec_recently_active`), **menu-2D coherence** (lines/flat-rects/mono-sprites squash on full-2D screens so dialog borders match the SPRT boxes; fades exempt), **HUD edge anchoring** (in-game untagged SPRTs pivot by thirds — outer-third elements keep their wide-screen corner position at native proportions). |
| `5151e5b` | **Authentic 4:3 BIOS boot** — squash held off until game entry PC fires (`fntrace_is_game_started`); Sony/PS logos + shell render 4:3, widescreen engages at game start. |
| TombaRecomp `124e3c9` | Tomba `[widescreen]` block (see config below). Tomba `game.toml` defaults `aspect_ratio = "4:3"`; the dev instance opts into 16:9 via `build-stable/settings.toml`. |
| `8ae73c6` / TombaRecomp `01478fd` | **2D parallax-backdrop screenX squash** (`psx_ws_backdrop_x`, `runtime/src/gpu.c`). The actor-table backdrop (far mountains, midground bush row) is overlay 2D sprites computing `screenX = (worldX-camX)>>parallax` in pure integer math — never touches the GTE, so the GTE squash misses it and far pieces clip past the 320 edge. Fix squashes that stored screenX around screen-centre by the same factor. Applied on BOTH paths: native via `[widescreen.backdrop] x_sites` recompiler emit (overlay DLLs via OverlayCallbacks, ABI v2→v3), and the dirty-RAM interp SH hook (the path that runs in dev). Sites: type-0 `0x801217B4`, type-1 `0x8012196C`. |
| `1e28a94` / TombaRecomp `66e13e5` | **Depth-gated far-backdrop GTE un-squash** (`gte_ws_set_suppress`, `runtime/src/gte.cpp`; `[widescreen.backdrop] unsquash_funcs` recompiler emit). The far ocean/cloud/distant-mountain is **GTE-3D** drawn by main-EXE `FUN_8004db3c`; the global squash compresses it toward centre → blue void at the revealed edges. Bracket that driver with a squash-suppress so its draws fill the frame skybox-style. Because the driver draws a MIX (far backdrop + near props), the suppress is **depth-gated**: only verts with projected SZ ≥ `s_ws_far_threshold` (default 900, live-tunable) un-squash; near props stay squashed/aligned. |

User has visually confirmed: world geometry, characters, HUD, menus, FMVs, and
(2026-06-13) the far-backdrop edge fill all present at correct (native)
proportions on the 16:9 stretch.

**Live diagnostics (TCP debug server, committed `1e28a94`/earlier):**
`ws_aspect num den` (toggle the GTE squash on/off in place — `1 1` = off),
`ws_far_threshold [t]` (set the SZ depth split + read the backdrop driver's SZ
stats so the split is data-driven), `ws_census on|off|<start> <end>` (always-on
per-prim draw ring → CSV), `ws_margin <v|-1>` (force the cull margin), and the
`gpu_state` `ws.{configured,active,game_mode,present_native_43,x_margin,squash}`
readout. Plus `tools/_press.py` (TCP pad), `tools/_objtab.py` (0x800A3D08 actor
table decoder).

---

## Config reference

```toml
[video]
aspect_ratio = "16:9"   # "4:3" (default/identity) | "16:9" | "21:9" | any "W:H"
                        # in [4:3, 32:9]. Wider than 4:3 engages the hack.

[widescreen]            # all optional; inert if absent
sprite_tag_funcs   = ["0x8005E08C"]   # guest addrs of the per-prim helper(s)
                                       # every character/billboard render fn
                                       # calls with the prim ptr in $a0.
sprite_anchor_addr = "0x1F800070"      # scratchpad holding the prim's
                                       # GTE-projected anchor SXY at tag time.
hud_sprt_squash    = true              # center/edge-squash untagged SPRTs
                                       # (screen-space HUD/menus).
auto_ui_squash     = true              # pre-scan the current GPU linked list,
                                       # select its highest populated UI rank,
                                       # and share one anchor across each
                                       # complete glyph/icon run.
clear_reveal       = true              # clear synthetic native-wide side margins
                                       # at opted-in scene/map boundaries (default false).
nw_left_hud_packet_lo = "0x000E3400"  # optional targeted left-HUD packet range
nw_left_hud_packet_hi = "0x000E4100"  # (half-open); avoids shifting 2D scenery.
offer_ultrawide    = true              # separate experimental 21:9 launcher row.
adaptive_view      = true              # expose live resize-driven aspect mode.

# Proven object/model participation comparisons may be forced only while a
# widened world view is configured. The complete instruction word guards
# same-address overlay variants; true 4:3 executes the vanilla comparison.
[[widescreen.cull.keep]]
address  = "0x8002B310"
expected = "0x28A21C01"                # SLT/SLTU/SLTI/SLTIU only
result   = 1                           # forced comparison result, 0 or 1
```

When `adaptive_view` is enabled by the game, the launcher's **Aspect ratio**
selector gains an **Adaptive** entry. The previously selected fixed aspect
still determines the initial window shape. Once the game window exists,
resizing it continuously
updates the GTE/native-wide projection, cull margins, wide render target, and
present aspect. The live ratio is clamped to 4:3 on the narrow side and to the
widest mode the game offers (`16:9`, or `21:9` with `offer_ultrawide`). BIOS,
FMV, menus, and other title-classified 2D frames retain their existing 4:3
pillarbox policy. The user choice persists as `[video] adaptive_view` in
`settings.toml`.

The native-wide draw-area early-out correction was independently identified
and contributed by **OpokXeno** in
[PR #73](https://github.com/mstan/psxrecomp/pull/73). The landed variant keeps
that mirror-space correction while restricting it to active framebuffer
targets, so offscreen texture targets retain the original host-side pruning.

Tomba's values are Ghidra-evidenced: `0x8005E08C` is the shared per-prim helper
all ~23 character render functions (RTPS cluster `0x800459E0`–`0x8004FB54`) call;
the RTPS preamble stores the anchor SXY to scratchpad `0x1F800070`.

`auto_ui_squash` is a runtime-only opt-in and does not require regenerated game
code. It applies only on the projection-and-stretch path: eligible
axis-aligned textured quads/rectangles in the front populated ordering-table
layer are grouped by texture and screen row before any command is transformed.
Depth-sorted world packets, full-frame backdrops, and true 4:3 frames remain
untouched.

**Changing `sprite_tag_funcs` requires a game regen** (the tag callback is
emitted into the generated C). `widescreen.cull.keep` is consumed by both the
native recompiler and the dirty-RAM interpreter: regenerate main/overlay native
code after changing it, while the interpreter reads the same guarded records
from the runtime config.

---

## Strategic direction: native-wide rendering (explored 2026-06-13, shelved)

Everything above is the **squash hack**: compress a wider FOV into the 320
framebuffer, present stretched. Its cost is a long tail of per-vertex-squash
artifacts (backdrop void, prop drift, the mountain grass-top straddle — see
ISSUES #8C). The **proper** fix is to stop squashing and render the wider FOV
**natively** into a wider framebuffer. No squash → that entire artifact class
disappears by construction (and it would likely fix the 8F dialogue split too).

A runtime PoC (`ws_native_wide` toggle) was built and **proved viable, then
reverted** (kept only as the checkpoint's history, not committed): drop the GTE
X-squash, widen the GPU draw-area + display by `WS_WIDE_EXTRA=106` (320→426 =
16:9 @ 240), and shift everything by `WS_WIDE_OFFSET=53` via `draw_offset` so 3D
and 2D move together and stay aligned. Result: content rendered un-squashed at
natural proportions with HUD/world alignment intact — the hypothesis held.

**Why a native-recomp project should prefer this:** we run on native hardware, so
**over-rendering off-screen geometry is essentially free** — we don't need to
cull the reveal precisely, just render more and not worry about the waste.

Remaining work to finish it (the PoC flushed these out):
1. **Widen the game's screen-space rejects** so the revealed side strips draw
   instead of being culled — e.g. `FUN_80027600`'s `SX>=0x140` (320) trivial
   reject. Over-render: set the bound generously (recompiler emit, regen-class).
2. **Paint the widened buffer region** — the game's framebuffer is 320 wide, so a
   426-wide display read shows stale VRAM where nothing was drawn (the PoC's
   right-edge garbage). The reveal must be covered (backdrop draws there once the
   cull is widened) and/or the region cleared.
3. **Re-anchor the 2D HUD/dialogue** to the wider frame (the `draw_offset` shift
   centres them; corner HUD needs to map to the true edges).

---

## Known issues / outstanding work

### 1. World-space culling pop-in — ROOT-CAUSED, patch set ready, NOT YET IMPLEMENTED

**This is the top remaining item.** Background scenery and some enemies pop
in/out while still on-screen in 16:9, because the game's per-object draw
classifier tests `objX − camX` against a **4:3-screen-derived window** that the
GTE squash never sees (it's a pre-projection world-space test).

Ghidra investigation (2026-06-12) located the entire classifier family and the
exact patch set. All constants are **code immediates** (no data tables).

- **camera X** (left edge of the 4:3 view) is mirrored each frame to scratchpad
  `*(u16*)0x1F800176` by the camera updater at `0x8002D90C` (adjacent to the
  H=544 re-assert at `0x8002D89C`).
- **Main draw classifier `FUN_80022E44`** (24 call sites — every "register for
  draw" path). Sets per-object draw flag at `obj+1`; the draw walker
  (`0x8004AFAC` / drainer `0x8004B1D8`) skips objects with `obj+1 == 0`
  (`0x8004B230`). Window math:
  ```
  0x80022E78: addiu v0,v0,0x40    ; +64 margin            <- widen
  0x80022E80: sltiu v0,v0,0x1C1   ; pass if < 449 (320+2*64+1)  <- widen
  ```
  With the 4/3 squash showing world `[-53.3, +373.3]`, the effective margin
  collapses to ~11 px → anything wider than ~22 px pops. **Draw-level, not
  despawn.**
- Widen by `E = 320*(4/3 − 1)/2 ≈ 54 (0x36)` symmetrically. Patch sites:

  | Addr | Fn | Current → New |
  |---|---|---|
  | `0x80022E78` | `0x80022E44` draw classifier | `addiu v0,v0,0x40` → `0x76` |
  | `0x80022E80` | " | `sltiu v0,v0,0x1C1` → `0x22D` |
  | `0x80022F94` | `0x80022F6C` predicate(obj) | `addiu v0,v0,0x40` → `0x76` |
  | `0x80022F9C` | " | `sltiu v0,v0,0x1C1` → `0x22D` |
  | `0x80022FE8` | `0x80022FD8` predicate(x,y) | `addiu a0,a0,0x40` → `0x76` |
  | `0x80022FF0` | " | `sltiu a0,a0,0x1C1` → `0x22D` |
  | `0x80023068` | `0x80023020` despawn (160 margin) | `addiu v0,v0,0xA0` → `0xD6` |
  | `0x80023070` | " | `sltiu v0,v0,0x280` → `0x2EC` |

  Param-margin variants (margin in `a1`, width `0x140` hard-coded) at
  `0x80022C08` / `0x80022D3C` / `0x800230BC`: convert the load-delay `nop`
  before the subtract (`0x80022C34` / `0x80022D68` / `0x800230FC`) into
  `addiu a1,a1,0x36` (`0x24A50036`) — hazard-free, inflates both bias and the
  `2*a1` bound. These are called from disc overlays (enemy AI) with no
  boot-EXE xrefs, but patching the shared main-EXE function covers all overlay
  callers.

- **Do NOT touch** `0x800344CC` (world→screen coord conversion).

**Recommended implementation (do NOT hand-patch generated code — Rule 4):**
make this a recompiler feature like `sprite_tag_funcs`. Add a `[widescreen]`
list of `(addr, kind)` cull-widen sites; emit the immediate as a
**runtime-computed term** `imm + psx_ws_x_margin()` where `psx_ws_x_margin()`
returns 0 at 4:3/boot and `~54` (aspect-derived) once engaged. That keeps one
build correct for both aspects and makes 21:9 automatic. The per-instruction
emit site is `CodeGenerator::translate_instruction` (`code_generator.cpp:670`);
the `addiu`/`sltiu` immediate handlers are where the substitution goes,
gated by the configured site set. **Requires a game regen.**

### 2. World-edge exposure (left side)

16:9 can reveal past the authored edge of an area's geometry. Candidate
gameplay clamps that pin an object to `camX+0x134`/`camX+0x136` (the 4:3 right
edge) live at `0x80057D94` / `0x80058580` — these are *gameplay* clamps, not
the cull, and widening them is a separate, lower-priority decision. Some areas
may simply have to accept a little edge reveal.

### 3. HUD edge anchoring is heuristic

In-game untagged SPRTs pivot by thirds (outer third → screen edge, middle →
centre). Works for Tomba's sparse corner HUD; a game with mid-screen HUD
elements could need per-element tuning. The "game vs full-2D screen" mode
detector (`ws_game_mode` in `gpu.c`) keys off "any tagged prim within 2 frames."

### 4. 21:9 untested

The launcher offers 21:9 and the math generalizes, but it has not been
playtested. Wider aspects worsen culling pop-in until issue #1 is fixed.

### 5. `sprite_tag_funcs` discovery is per-game manual

Finding the shared per-prim helper + anchor scratch requires a Ghidra pass per
title. Documented for Tomba; any new title needs the same investigation.

---

## Key files

- `runtime/src/gte.cpp` — `gte_set_display_aspect`, RTPS/RTPT X-squash.
- `runtime/src/gpu.c` — `gpu_ws_configure`, `psx_ws_sprite_tag`, tag table,
  per-prim transforms in the GP0 exec handlers.
- `runtime/src/gpu_gl_renderer.c` — `gl_renderer_set_display_aspect`,
  `letterbox_rect_aspect`, `force_4_3` present paths.
- `runtime/src/main.cpp` — config resolution, `g_ws_engaged` (boot gate),
  `fmv_frame` 4:3 pin, window sizing.
- `runtime/src/mdec.c` — `mdec_recently_active` (FMV detector).
- `recompiler/src/code_generator.cpp` — `psx_ws_sprite_tag` emit at tagged fn
  entry. **This is where the cull-widen emit (issue #1) belongs.**
- `recompiler/src/config_loader.{h,cpp}` — `[widescreen]` parse, `aspect_ratio`.
- Launcher aspect control — aspect toggle.
- `TombaRecomp/tools/capture_game_window.ps1` — window-capture helper for
  visual verification (the VRAM `screenshot_file` shows the pre-stretch frame;
  this shows the post-stretch window).

## Verify / rebuild

Runtime-only changes (everything except `sprite_tag_funcs` / cull-widen) just
need a rebuild:
```
PATH=/c/msys64/mingw64/bin:$PATH
cmake --build ../TombaRecomp/build-stable --target psx-runtime -j16
```
Changes that emit into generated C (`sprite_tag_funcs`, future cull-widen) need
a regen first (see `memory/regen_build_recipe.md`):
```
recompiler/build/psxrecomp-game.exe --config game.toml
```
`build-stable` is RelWithDebInfo + debug tools (TCP port 4470) + launcher ON.
Always `taskkill /F /IM psx-runtime.exe` before relaunching.

## Native-wide 16:9 (shipped) and 21:9 (stubbed, hidden)

The shipped enhancement is **native-wide 16:9**, exposed as the launcher's
experimental Widescreen On/Off toggle (software renderer only; hidden on
OpenGL). It is off by default (`game.toml [video] aspect_ratio = "4:3"`), so the
default build is byte-identical to faithful. On = `16:9`. The dominant fix is
`[widescreen.cull] auto_screen_x` — the recompiler auto-detects the GTE
render-funnel screen-extent reject (`sltiu …,0x140` width paired with
`sltiu …,0xE0` height) and widens every width compare through
`psx_ws_x_margin()`, sign-extended so both margins reveal. Full-screen 2D
overlays (pause dim, load fade) are extended across the wide compositor surface.

**21:9 is intentionally stubbed and hidden.** Everything numeric is
aspect-derived (`ws_nw_offset` etc.), so `aspect_ratio = "21:9"` already widens
the cull, compositor, and present correctly — the **3D world and foreground fill
21:9 for free**. It is *not* offered in the UI because two layers don't reach
21:9:
- **Background** — the parallax + far-backdrop pipeline only *generates* ~16:9
  of coverage (per-layer spawn windows in the `FUN_80027600` builders +
  `FUN_801216BC`-family overlay handlers). The per-prim cull already widens past
  this; the tiles simply aren't produced, so 21:9 voids the far background. This
  is the 8C generation work, and may hit the game's *authored* coverage limit
  (beyond which only tile/stretch/void remain) — which is what caps clean
  widescreen at 16:9.
- **Foreground edge tips** — the cull widens exactly to the display surface
  edge; fixable by over-rendering (decouple the render aspect from the display
  aspect) — low effort, but pointless until the background is solved.

To enable 21:9 later: widen the backdrop generation, then promote the launcher's
2-state toggle to a 3-way Off / 16:9 / 21:9 (the `cycle_aspect` callback already
cycles `aspect_index` 0/1/2 and is the scaffold). Full feasibility analysis in
the session memory `native_wide_fov_autocull.md`.
