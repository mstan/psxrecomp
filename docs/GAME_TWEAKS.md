# Game tweaks without per-game forks

This guide explains how a game project can ship widescreen, 60 FPS, camera,
physics, and related enhancements without maintaining its own psxrecomp fork.

## Choose the right extension point

The Einhander, Tobal 2, and Mega Man Legends forks contain three kinds of work:

1. General runtime fixes, which belong in the shared runtime.
2. Reusable renderer/runtime capabilities selected by `game.toml`, such as the
   existing widescreen, culling, backdrop, controller, and pacing options.
3. Exact guest-code changes, such as gameplay bounds or 30-to-60 FPS divisors.

Use the narrowest shared mechanism that fits:

| Change | Put it here |
|---|---|
| Generally correct runtime/recompiler fix | Shared psxrecomp source plus cross-title tests |
| Reusable widescreen/render/input behavior | Typed, off-by-default `game.toml` option |
| Exact game instruction change | `[[recompiler.patch]]` in the game repository |
| Behavior requiring live state or several instructions | New typed config feature, not injected source text |

## Quick start

Keep guarded replacements beside the game's own configuration instead of
adding game IDs and addresses to `code_generator.cpp`:

```toml
[[recompiler.patch]]
id = "60fps-update-divisor"
address = "0x80012340"
expected = "0x24020002"    # addiu v0, zero, 2
replacement = "0x24020001" # addiu v0, zero, 1
note = "Advance gameplay on every vblank"
```

Then regenerate normally:

```sh
psxrecomp/recompiler/build/psxrecomp-game --config game.toml
cmake -S . -B build -G Ninja
cmake --build build
```

The replacement happens before normal MIPS-to-C translation, so every supported
instruction uses the existing translator and runtime. No runtime patch engine,
writable-code trick, or game-specific C callback is involved.

## Patch fields

| Field | Required | Meaning |
|---|---:|---|
| `id` | yes | Stable, descriptive name used in diagnostics |
| `address` | yes | Virtual address of the instruction as a hex string |
| `expected` | yes | Exact original 32-bit MIPS word as a hex string |
| `replacement` | yes | Exact replacement MIPS word as a hex string |
| `note` | no | Why the enhancement needs this change |

Addresses must be unique in one configuration. Code generation aborts unless
the instruction at `address` exactly equals `expected`. Combined with the
normal disc hash in `game.toml`, this prevents a patch for one revision from
silently corrupting another.

For example, a stale address produces a diagnostic shaped like:

```text
ERROR: recompiler patch '60fps-update-divisor' expected 0x24020002 at
0x80012340, found 0x8C820000. Wrong game revision or stale patch.
```

## Common examples

### 30 FPS to 60 FPS divisor

After proving that the game advances logic every second vblank, replace only
the divisor/load responsible for that policy:

```toml
[[recompiler.patch]]
id = "gameplay-60fps-divisor"
address = "0x80042A10"
expected = "0x24080002"    # addiu t0, zero, 2
replacement = "0x24080001" # addiu t0, zero, 1
note = "Run the main gameplay update each vblank"
```

This does not assert that one instruction is sufficient for every game. Audio,
animation, physics, timers, and FMVs must still be validated. Add separately
named guarded entries for each understood correction.

### Remove a game-side 4:3 camera clamp

```toml
[[recompiler.patch]]
id = "wide-camera-right-limit"
address = "0x8008B244"
expected = "0x3C040153"
replacement = "0x3C0401C4"
note = "Increase the proven Q16 right limit for the 16:9 build profile"
```

Use this only when the enhancement is build-time and the replacement is valid
for that whole build. If the value must switch live when the player changes
between 4:3 and 16:9, add a typed widescreen transform that is identity in 4:3;
do not bake a permanently wide value.

For a signed Q16 `lui` bound that must follow the live aspect ratio, use the
typed transform instead. It is shared by static generation, captured-overlay
generation, the overlay JIT, and the dirty-RAM interpreter:

```toml
[[widescreen.signed_x_bound]]
address = "0x800AB0CC"
expected = "0x3C040153" # lui a0, 0x0153 (positive Q16 bound)

[[widescreen.signed_x_bound]]
address = "0x800AB0E4"
expected = "0x3C04FEA7" # lui a0, 0xFEA7 (negative Q16 bound)
```

The helper returns the original signed value outside native-wide gameplay and
scales it by the active horizontal field during native-wide gameplay. The
expected instruction must be `lui`; other shapes are rejected by the loader.

### Combine patches with reusable widescreen features

```toml
[video]
aspect_ratio = "16:9"

[widescreen]
gte_game_mode = true
nw_hud_corners = true
nw_backdrop = true

[widescreen.cull]
auto_screen_x = true

[[recompiler.patch]]
id = "wide-camera-limit"
address = "0x80045678"
expected = "0x24020140"
replacement = "0x240201C7"
note = "Title-specific camera limit; rendering and culling remain generic"
```

The renderer features remain reusable and inert for games that do not opt in;
only the final title-specific guest constant belongs in the patch table.

### Expand finite textured arena edges

Some 3D arenas already submit textured polygons beyond the 4:3 edges, but not
far enough to cover 16:9. This typed renderer feature expands only vertices
already outside the canonical boundary and preserves centre geometry:

```toml
[widescreen]
nw_textured_edges = true
nw_textured_edge_scale = 200 # 100..400; 0 selects aspect-derived scaling
nw_full_mirror = true        # preserve interpolation across the old boundary
```

Sprites, rectangles, HUD, actors inside the canonical field, and the faithful
4:3 path remain unchanged. `nw_full_mirror` is needed when an edge-crossing
polygon would otherwise be spliced with canonical centre pixels.

## Optional enhancement profiles

Patches are build-time inputs. To ship faithful and enhanced binaries, keep two
complete configurations in the game repository, for example:

```text
config/
  game-faithful.toml
  game-enhanced.toml
```

The faithful file omits enhancement patches. The enhanced file contains them
and selects the corresponding widescreen/video defaults. Generate each into a
separate `out_dir` so outputs cannot overwrite one another. If a setting must be
toggled inside one binary, it needs a typed runtime feature rather than two
static instruction words.

## Main executable and overlays

Normal main-EXE generation receives patches through `--config game.toml`.
Overlay compilation must receive the same configuration through the existing
`--ws-config game.toml` path. Overlay addresses may contain different code in
different scene variants; the expected opcode remains the authority. A patch
should be scoped only to a captured variant whose bytes are understood. Do not
remove the opcode guard to make a mismatched overlay compile.

## Finding instruction words

Use the game's disassembly or executable reader to record:

1. The virtual instruction address.
2. The exact original little-endian 32-bit word.
3. The replacement word and decoded instruction.
4. Evidence explaining why the site controls the intended behavior.
5. Validation covering fidelity mode and the enhancement.

Keep that evidence in the game repository. Patch IDs and notes should describe
intent, not merely repeat the address.

## Migration checklist

- Inventory the game fork against shared psxrecomp.
- Promote generally correct fixes into the shared runtime with cross-title tests.
- Express reusable graphics behavior as an off-by-default config capability.
- Express small guest-code changes as guarded instruction patches.
- Delete superseded project-carried patch files and hard-coded game-ID branches.
- Keep patch entries and their evidence in the game repository.
- Regenerate all affected main/overlay C outputs.
- Build faithful and enhanced variants from clean directories.
- Validate boot, gameplay, audio, input, saves, FMVs, and frame pacing as relevant.
- Confirm a deliberately wrong `expected` word fails generation loudly.

This does not accept arbitrary source snippets or host callbacks. If an exact
replacement is insufficient, add a typed, validated schema feature with an
inert default rather than recreating an unsafe plugin-shaped fork boundary.
