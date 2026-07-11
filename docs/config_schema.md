# PSXRecomp v4 — config TOML schema

Consumed by:
- `psxrecomp-bios.exe` and `psxrecomp-game.exe` (via `--config <toml>`)
- `tools/audit_config.py` and the audit tooling that wraps it
- The runtime cmake macro (`runtime/runtime.cmake`)

Examples:
- `bios/SCPH1001.toml` — BIOS-only config (no game; psx-runtime targets this)
- `../TombaRecomp/game.toml` — game config (tomba-runtime targets this)

## How configs combine

A PSXRecomp v4 process **always** has a BIOS config. It optionally also has
a game config. Both are TOML files in this schema:

- `bios/SCPH1001.toml` (or another BIOS .toml in `bios/`) — describes the
  BIOS. Always loaded.
- `<game>/game.toml` — describes a single game. Loaded ONLY when running
  that game.

```
psxrecomp                            # BIOS-only regen (uses default bios.toml)
psxrecomp games/tomba/game.toml      # BIOS + game regen
psx-runtime                          # boots BIOS discless
psx-runtime games/tomba/game.toml    # boots BIOS, then loads game
```

When both are loaded, keys merge:

- **Scalar keys (`debug_port`, `window_title`, `memcard_dir`, ...)**: game
  wins if set, inherit from bios.toml otherwise. Shallow override.
- **`[program]` (BIOS) and `[game]` blocks**: NOT merged — they describe
  different programs. Both are visible to the loader.
- **Generated dispatch tables and C output**: ADDITIVE. BIOS contributes
  `SCPH1001_*.c`; game contributes `<exe>_*.c`. No address overlap is
  expected (BIOS lives at RAM 0x500-0x8500 + ROM 0xBFC..., game at
  0x80010000+). The cmake macro `psxrecomp_v4_add_runtime_target` already
  links both.
- **`[[audit.regions]]` and `[[audit.normalize.remap]]`**: additive — game
  adds its regions on top of BIOS's.

## Top-level blocks

```toml
[program]    # in bios.toml; describes the BIOS
[game]       # in game.toml; describes the game
[recompiler]
[runtime]
[audit]
```

## Program / game block

- **bios.toml** has a `[program]` block describing the BIOS ROM.
- **game.toml** has a `[game]` block describing the game EXE / disc.

These are NOT alternatives; they're complementary. A runtime loading both
sees both blocks. The legacy single-file audit loader
(`tools/audit_config.py`) accepts either as the program-info source for
backwards compat, but going forward they are the canonical names for their
respective files.

### Fields

| Field | Required for | Description |
|---|---|---|
| `name` | both | display name, e.g. `"SCPH1001 BIOS"` |
| `id` | both | canonical id, e.g. `"SCPH-1001"` or `"SCUS-94236"` |
| `rom` | bios | path to raw flat binary, relative to project root |
| `exe` | game | path to PS-X EXE file, relative to project root |
| `load_address` | both | hex string, virtual address of first byte (`"0xBFC00000"` BIOS, `"0x80010000"` typical game) |
| `entry_pc` | both | hex string, first PC to execute |
| `text_size` | both | hex string, size in bytes of code segment (used by audits to bound region walks) |
| `stack_base` | game | hex string, initial `$sp` value for the game |
| `disc` | game (single-disc) | path to .cue, relative to project root |
| `discs` | game (multi-disc) | array of .cue paths; `disc` is sugar for `discs = [disc]` |

## Recompiler block

```toml
[recompiler]
seeds       = "recompiler/seeds/phase2_ghidra_seeds.json"  # BIOS
seeds       = "seeds/ghidra_funcs.txt"                     # game (note: game seeds aren't json today)
bios_thunks = "seeds/tomba_bios_thunks.txt"                # game-only
out_dir     = "generated"                                  # both
strict      = true                                         # both — currently always true
out_stem    = "SCPH1001"                                   # optional; overrides the auto-derived stem

[[recompiler.patch]]
id          = "gameplay-60fps-divisor"
address     = "0x80012340"
expected    = "0x24020002"
replacement = "0x24020001"
note        = "Run the gameplay update every vblank"
```

Output filenames: `<out_dir>/<out_stem>_full.c` and
`<out_dir>/<out_stem>_dispatch.c`. If `out_stem` is omitted, it's derived
from the `rom`/`exe` file basename with the trailing `.BIN` or `.EXE`
stripped (`Path.stem` is NOT used because it mishandles `SCUS_942.36`).

Each `[[recompiler.patch]]` performs one exact MIPS word replacement before
translation. All fields except `note` are required. Duplicate addresses are
rejected. Main-EXE generation aborts if the executable word differs from
`expected`; overlay generation skips nonmatching variants at the same virtual
address and applies the patch only to the variant whose word matches.
Use these for small, understood timing/bounds/logic changes; prefer typed
features such as `[widescreen]` for reusable runtime behavior.

### Typed widescreen extensions

```toml
[widescreen]
nw_textured_edges = true
nw_textured_edge_scale = 200 # 0 or 100..400
nw_full_mirror = true

[[widescreen.signed_x_bound]]
address = "0x800AB0CC"
expected = "0x3C040153" # must decode as LUI
```

`nw_textured_edges` expands only textured polygon vertices already beyond the
canonical 4:3 boundary. `nw_full_mirror` disables the centre-splice fast path
when transformed edge-crossing interpolation must remain continuous.

Each `signed_x_bound` entry identifies a guarded `lui` whose signed Q16 value
must scale with the active native-wide horizontal field. It is identity outside
native-wide gameplay and applies consistently in static code, compiled overlay
variants, SLJIT overlays, and dirty-RAM interpretation.

## Runtime block

Consumed by the cmake macro `psxrecomp_v4_add_runtime_target` (eventually)
and by `runtime/src/main.cpp` as the source of compiled-in defaults.

```toml
[runtime]
debug_port    = 4370            # TCP port for the debug server
window_title  = "..."           # SDL window title
controller    = "digital"       # "digital" or "dualshock"
memcard_dir   = "."             # memcard files location, relative to project root
```

Reserved future fields:
- `default_disc_path` — game runtimes can pre-mount a disc
- `default_game_root` — for sibling-junction setups

## Audit block

See `docs/audit_inventory.md` for the audit pipeline. The schema here is
the input side: regions to walk, address-normalisation rules.

```toml
[audit]
function_starts = "generated/ghidra_function_starts.json"   # optional

[[audit.regions]]
name        = "..."             # e.g. "Boot", "Kernel", "Shell", "Text"
rom_start   = "0x..."           # byte offset in rom/exe file
rom_end     = "0x..."           # exclusive
vaddr_base  = "0x..."           # virtual address corresponding to rom_start

[audit.normalize]
kseg_mask = "0x1FFFFFFF"

[[audit.normalize.remap]]
description = "..."             # human-readable; not consumed by tooling
from_lo     = "0x..."           # inclusive
from_hi     = "0x..."           # exclusive
to_lo       = "0x..."           # target start offset (phys = phys - from_lo + to_lo)
```

## What's NOT in the schema yet (Phase B+)

These are noted here so future work knows where to slot them:

- Game `discs` field (Phase D). For now Tomba uses `disc = "..."`.
- `[runtime] disc_swap_command` — runtime-side disc swap (Phase D).
- `[recompiler] seeds` as an array of paths (currently single file;
  Phase A might allow multiple).
- `[program] type` explicit discriminator (currently inferred from
  `rom` vs `exe` field presence).
