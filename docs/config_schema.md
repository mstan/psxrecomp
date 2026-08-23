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

How the two configs relate:

- **Scalar keys (`debug_port`, `window_title`, `memcard_dir`, ...)**: these come
  from `game.toml` only. There is **no BIOS→game inheritance.** An earlier
  version of this document described a shallow override where the game won and
  otherwise inherited from `bios.toml`; that merge was never implemented.
  `load_bios_config` (`recompiler/src/config_loader.cpp`) is called only from the
  recompiler front-ends — `main_bios.cpp`, and `main_psx.cpp` purely to build the
  `BiosAddressModel` — and never from `runtime/src/main.cpp`. Setting a
  `[runtime]` scalar in a BIOS toml has no effect on a game run.
  Runtime precedence is: environment > CLI > `settings.toml` > `game.toml` >
  compiled-in default.
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
[prepare_disc]  # optional; data-track digests for prepare/verify
[netplay]       # optional; TOC / cue policy for online
[recompiler]
[runtime]
[audit]
```

## Netplay disc mount (`[netplay]`)

Optional. Online play needs the same CD geometry on every peer — data-track
CRC/SHA alone cannot distinguish a Track-01-only dump from a full Redump
multi-track cue. The runtime mounts the resolved path, fingerprints the TOC
(`disc_fp`), and gates Host/Join on `netplay_ok`. Peers also exchange
`disc_fp` through the lobby (`disc_mismatch` on join).

| Field | Default | Description |
|---|---|---|
| `require_cue` | `false` | Require a `.cue` mount (reject bare `.bin` / cue→bin fallback) |
| `required_tracks` | `0` | Exact `iso_track_count` when > 0 (e.g. MotK Redump = `17`) |
| `required_leadout_lba` | unset | Exact lead-out LBA when set |
| `required_disc_fp` | `""` | Exact lowercase hex SHA-256 TOC fingerprint when non-empty |

Offline Play may still launch with a TOC warning; first-run setup Finish and
online Create/Join require `netplay_ok` (and online also a clean verify +
non-empty `disc_fp`). Mirror `required_tracks` in the RetComM catalog as
`rom_identity.track_counts` so the hub library scan rejects Track-01-only dumps.
Wizard / RetComM / catalog submission accept Redump `.cue` + sibling `.bin`
tracks only — not `.iso`/`.chd` (cannot reliably expand to multi-track).

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
| `text_size` | both | hex string, size in bytes of the static region. For games this also bounds main-EXE analysis and establishes the overlay floor. A smaller-than-header bound must be verified non-code and 4 KiB aligned. |
| `stack_base` | game | hex string, initial `$sp` value for the game |
| `disc` | game (single-disc) | path to .cue, relative to project root |
| `discs` | game (multi-disc) | array of .cue paths; `disc` is sugar for `discs = [disc]` |

## Recompiler block

```toml
[recompiler]
seeds       = "recompiler/seeds/phase2_ghidra_seeds.json"  # BIOS
seeds       = "seeds/ghidra_funcs.txt"                     # game (note: game seeds aren't json today)
bios_thunks = "seeds/tomba_bios_thunks.txt"                # game-only
bios_config = "psxrecomp/bios/SCPH1001.toml"               # game-only: BIOS profile whose address
                                                           # model game codegen folds RAM aliases
                                                           # through (see "BIOS profiles" below);
                                                           # defaults to the SCPH1001 profile
out_dir     = "generated"                                  # both
strict      = true                                         # both — currently always true
discovery   = "whole-image"                                # game-only: "whole-image" or "reachable"
out_stem    = "SCPH1001"                                   # optional; overrides the auto-derived stem

[[recompiler.patch]]
id          = "descriptive-policy-name"
address     = "0x80012340"
expected    = "0x24020002"
replacement = "0x24020001"
note        = "Why this game-owned instruction change is required" # optional
```

Output filenames: `<out_dir>/<out_stem>_full.c` and
`<out_dir>/<out_stem>_dispatch.c`. If `out_stem` is omitted, it's derived
from the `rom`/`exe` file basename with the trailing `.BIN` or `.EXE`
stripped (`Path.stem` is NOT used because it mishandles `SCUS_942.36`).

Game `discovery` defaults to `"whole-image"`, preserving the existing sweep and
pointer-table heuristics. Opt-in `"reachable"` starts at the executable entry
and evidence-backed seed roots, then follows callable direct `jal` targets. It
does not sweep arbitrary bytes for prologues or return-shaped words. Unresolved
`jalr`/indirect targets and unseen callbacks fail closed to runtime
interpretation; add an evidence-backed seed (or a `dispatch_root` seed for a
proven nonstandard boundary) when they should be compiled.

When `game.text_size` is smaller than the PS-X EXE header size, recompilation
uses it as a static-analysis bound. The value must be nonzero, instruction- and
4 KiB-aligned, retain `entry_pc`, and not extend past PS1 RAM. The original EXE
is still loaded from the user's disc. A generated config's canonical final-page
reservation may be slightly larger than the header; it does not widen analysis.

Each `[[recompiler.patch]]` replaces one exact 32-bit MIPS word before function
discovery, control-flow analysis, and normal translation. It is intended for
small, understood game-code changes whose addresses, opcodes, and policy remain
in the game repository. The framework does not contain title IDs or
title-specific addresses.

- `id`, `address`, `expected`, and `replacement` are required hex/string
  fields; `note` is optional.
- IDs are case-sensitive and unique within one config.
- Addresses must be four-byte aligned and are unique by the PSX 29-bit physical
  address. Thus `0x00012340`, `0x80012340`, and `0xA0012340` are aliases of one
  site and cannot define separate patches.
- Main-EXE generation fails if the word at the target site is not `expected`.
  This catches a wrong disc revision or stale patch instead of guessing.
- Captured overlays may place unrelated variants at one virtual address. In
  overlay mode, a patch is applied only to a variant whose word is `expected`;
  a nonmatching variant is translated unchanged.
- When `--config` and `--ws-config` supply the same byte-identical patch it is
  deduplicated. Reusing an ID or physical address for different patch data is
  an error.

Patches are build-time inputs, not runtime memory writes or live toggles.
Regenerate the affected main executable or captured overlays after changing
them.

### Guarded widescreen participation comparisons

Games may disable a proven object/model cull verdict in widened world views
without changing true 4:3 behavior:

```toml
[[widescreen.cull.keep]]
address = "0x8002B310"
expected = "0x28A21C01"
result = 1
```

- `expected` must encode `SLT`, `SLTU`, `SLTI`, or `SLTIU`.
- `result` must be 0 or 1.
- The site identity is the normalized physical address plus the complete
  32-bit instruction. A nonmatching overlay variant at the same VA is left
  unchanged.
- At true 4:3 the original comparison is evaluated. The configured result is
  forced only when `psx_ws_x_margin() > 0`.
- Native generated code and the dirty-RAM interpreter implement the same
  semantics.
- Regenerate main/overlay native code after changing the list.

Prefer an aspect-derived cone over `keep` when the original predicate is a
camera-frustum test. `keep` has no queue policy and is appropriate only for a
separately proven binary verdict.

### Aspect-aware terrain and model participation

Exact terrain-frustum angle loads can follow the live horizontal field:

```toml
[[widescreen.cull.angle]]
address = "0x8013F138"
expected = "0x24020155"
```

`expected` must be `ADDI`/`ADDIU rt,zero,imm`, with a positive 12-bit angular
half-extent below one quarter-turn. The helper widens `tan(angle)` by the live
per-side horizontal extent. It is exact at 4:3 and full-word guarded against
same-address overlay variants.

A model-list or per-child cosine rejection can use a horizontal-only envelope:

```toml
[widescreen.cull.aspect_cone]
forward_addr = "0x1F8000E8" # signed Q12 X/Z/Y halfwords
object_type_offset = 12
object_reg = 19
x_reg = 16
z_reg = 17
y_reg = 18
hysteresis_pixels = 24
queue_reserve = 4
queue_count_addrs = ["0x1F800144", "0x1F800150", "0x1F80015C"]
queue_capacities = [24, 40, 28]
queue_type_masks = ["0x00000204", "0x00000010", "0x00000020"]

[[widescreen.cull.aspect_cone.sites]]
address = "0x80077368"
expected = "0x28620358" # signed SLTI reject predicate

[[widescreen.cull.aspect_cone.sites]]
address = "0x8002B368"
expected = "0x0082202A" # signed SLT reject predicate
cosine_threshold = 856  # required for SLT; Q10
object_reg = 20         # optional per-site register overrides
x_reg = 19
z_reg = 18
y_reg = 17
queue_guard = false     # this lower-level predicate appends to no fixed queue
```

- Sites must be signed `SLTI` or `SLT` reject predicates: zero is the keep
  path and one is rejection.
- An `SLTI` site derives its Q10 cosine threshold from the immediate unless
  `cosine_threshold` is given. An `SLT` site requires it explicitly.
- A vanilla keep is always preserved. Only a vanilla rejection is retested.
- Horizontal reach follows the current client aspect. Vertical reach,
  near/far checks, type dispatch, and the game’s queue-capacity branches are
  unchanged.
- `guard_pixels` is the activation guard outside the visible field.
  `hysteresis_pixels` moves deactivation farther out.
- For `queue_guard = true`, non-visible guard/hysteresis candidates are
  rejected at `capacity - queue_reserve`, preserving headroom for candidates
  intersecting the visible wide field.
- Use `queue_guard = false` only after proving that the exact predicate does
  not append to those queues.
- Site address, instruction, threshold, registers, queue policy, guard size,
  and enclosing cone/queue metadata all contribute to overlay cache identity.
- Generated/native overlay code and the dirty-RAM interpreter use the same
  live helper. Dynamic resizing therefore needs no recompilation.

The debug server’s `ws_aspect_cone_site` command accepts an `address` string
and reports exact-site identity/keep/reject counters.

Explicit `bias_sites` / `range_sites` may opt into an additional resident
object lead without widening terrain or render queues:

```toml
[widescreen.cull]
guard_pixels = 16
activation_guard_pixels = 256
bias_sites = ["0x80069BA8"]
range_sites = ["0x80069BB0"]
```

`activation_guard_pixels` is added only to the live margin emitted at those
two explicit site families, and only while widescreen reveals extra world.
At true 4:3 it is exactly zero. `guard_pixels` remains the shared
render/terrain participation guard; keep it small when terrain producers or
model queues have fixed capacity. Both values are restricted to `[0, 256]`
and contribute to native-overlay cache identity. Changing the activation
guard requires regenerating the game and overlay code.

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

### Opt-in warm CD routes

Per-game read acceleration is disabled unless the game explicitly declares
one or more strict routes. Each route arms on one `SetLoc`, then requires every
later file start to match `lbas` in order. A mismatch immediately restores the
normal configured disc timing. Only data-read cadence is accelerated; XA/CDDA,
seek, and motor timing remain authentic.

```toml
[[runtime.warm_cd_routes]]
arm_lba = 95947
lbas = [298, 299, 306]
instant_max_per_frame = 32
```

Up to 16 routes may be declared, with 1–64 LBAs each. The old singular
`[runtime.warm_cd_route]` table is deprecated and emits a warning when loaded;
it remains readable for compatibility. This enhancement is intentionally
opt-in and must not acquire a global default.

The other load-time accelerators are likewise opt-in:

```toml
[runtime]
idle_skip = true
turbo_audio_sink = true
```

### SIO1 serial link (`[runtime.link]`)

The SIO1 register file at `0x1F801050..0x105F` (link cable port) is always
live — every PlayStation has the registers whether or not a cable is plugged
in (`accuracy/axis4_sio1_serial.md`; env `PSX_SIO1_REGS=0` restores the legacy
reads-0 decode for A/B). This block configures the **peer** on the far side of
the cable, default none:

```toml
[runtime.link]
enabled        = false     # attach a link peer (default off = no cable)
backend        = "null"    # "null" | "loopback" | "crossover"
latency_cycles = 0         # deterministic extra wire delay per character
trace          = false     # reserved: SIO1 character trace ring
```

- `null` — no cable: DSR/CTS read low, TX shifts into the void.
- `loopback` — self-wired test cable (TX→own RX, DTR→own DSR): validates a
  title's link path without a second console.
- `crossover` — reserved for the dual-console driver, which cross-wires two
  in-process machines itself; selecting it without that driver behaves as
  `null`.

All backends are deterministic (no wall clock, no sockets on the sim path)
and rollback/savestate-safe; wire state snapshots into `BS_SEC_SIO1`. Env
overrides for A/B: `PSX_SIO1_BACKEND`, `PSX_SIO1_LATENCY`.

### `turbo_loads` / `offer_turbo_loads` — deprecated and ignored

**Do not use these keys.** Load acceleration is owned by the Mods catalog:
`psx.enhancement.fast-loading` ("Fast Loading (host pacing)") and
`psx.enhancement.cd-speed`. Both target `game_id = "*"`, so they ship with every
title, both default to off, and both expose the multiplier and instant-scheduler
detail that a single opaque boolean never could. recomp-ui correspondingly draws
no generic Turbo loads row.

Both keys are still parsed so existing configs load without error, but neither is
honoured — the runtime logs a deprecation line naming the Fast Loading mod and
leaves acceleration off. Remove them from `game.toml`.

The same applies to `[video] turbo_loads` in a user's `settings.toml`: it is no
longer restored at startup, and it is no longer written back out, so the stale
row disappears on the first save after updating. This is deliberate. Because the
launcher stopped drawing a control for it, a persisted `true` was simultaneously
authoritative and unreachable: one run of a build whose `game.toml` said `true`
latched turbo on permanently, and no later config change could undo it. That
shipped to players in MegaManX6Recomp v1.0.4/v1.0.5 (MegaManX6Recomp#14). Never
restore this row without also restoring a UI control for it.

For development, the `turbo_loads` TCP debug command still toggles acceleration
at runtime.

`turbo_audio_sink` is meaningful only while load acceleration is active. It keeps
the guest SPU timeline advancing but discards accelerated samples before host
playback, then fades normal output back in.

## Audio Block

Game projects may choose the host playback cushion after validating their
audio production cadence:

```toml
[audio]
buffer_ms = 60
```

`buffer_ms` accepts 30–500 milliseconds and defaults to 180. Lower values
reduce audible input-to-sound delay, but leave less reserve for frames where a
game temporarily produces no audio and can therefore crackle on affected
titles. This is deliberately a per-game developer choice; it is not read from
the player's `settings.toml`.

## Video Block

Runtime video defaults live in `[video]`:

```toml
[video]
renderer = "opengl"       # "software", "opengl", or "vulkan"
offer_vulkan = false      # show Vulkan in the launcher only after game validation
auto_skip_fmv = false     # legacy Settings/runtime default
offer_skip_fmv = true     # false when the game exposes this through Mods
```

`renderer = "vulkan"` remains an experimental runtime choice and still requires
a build compiled with Vulkan support. `offer_vulkan` controls launcher
visibility only; it defaults to false so game projects must explicitly expose
Vulkan after validating their visuals and stability.

`offer_skip_fmv` defaults to true for compatibility with the shared PSX
Settings surface. A game migrating Skip FMVs into its built-in mod catalog sets
it to false. The runtime then hides the Settings row, ignores stale persisted
values, and leaves activation to the selected trusted plugin.

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

## BIOS profiles (`bios/<STEM>.toml`)

One profile per BIOS image; the profile is the single source of truth for the
image identity, the relocation windows, and the runtime anchors. Two ship:
`bios/SCPH1001.toml` (retail; user supplies the dump) and `bios/OpenBIOS.toml`
(MIT, redistributable, shipped with the build). Normal runtimes link both
generated backends (`PSXRECOMP_BIOS_STEMS=OpenBIOS;SCPH1001`) and select one at
launch. The recompiler's `[recompiler] bios_config` identifies the profile used
for game code generation; it does not choose the player's runtime BIOS.

```toml
[program.image]              # identity; recompiler refuses a mismatched ROM
sha256          = "..."      # pins the exact image (empty = unchecked)
redistributable = false      # true: BIOS ships with the game; runtime hides
                             # any requirement to provide that image

[recompiler.address_model]   # boot-time bulk code copies out of ROM
normalize_mask = "0x1FFFFFFF"
[[recompiler.address_model.copy]]
name         = "Kernel Part 2"   # comment label in the emitted C
rom_lo       = "0x1FC10000"      # [lo, hi) physical, hi EXCLUSIVE
rom_hi       = "0x1FC18000"
ram_lo       = "0x00000500"      # physical RAM destination
runtime_base = "0x00000500"      # vaddr the CPU executes the copy at
dispatch_key = "ram"             # "ram": functions keyed by RAM address;
                                 # "rom": RAM alias folds back to ROM
kernel_bless = true              # runtime may byte-verify + run native

[[recompiler.install_slots]] # kernel-RAM PCs the BIOS patches at runtime
ram_addr = "0x00000CF0"

[recompiler.runtime_exports] # per-image HLE anchors (omit = unavailable)
shell_entry_phys  = "0x00030000"
deliver_event_ret = "0x80001720"
```

Every `copy` entry is a claim that the boot copy is byte-verbatim; the
runtime kernel-bless memcmp enforces it. A BIOS with no copies (runs
entirely from ROM) is valid: normalization degenerates to the KSEG mask.
Semantic invariants (disjoint windows, no fold-output/input intersection,
single bless window) are enforced at load; violations refuse to build.
