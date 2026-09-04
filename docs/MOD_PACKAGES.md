# PSXRecomp mod packages and features

A `.psxmod` is a versioned installation, provenance, and trust boundary. A
package may contribute any number of independently configurable **features**.
The launcher presents those features as the primary Mods list; package
installation, version selection, and removal are a secondary management view.

Feature identity is always `(package_id, feature_id)`. Enabling one feature
never enables, disables, or reconfigures another feature.

The player selects a verified stock BIN/CUE. Resolution produces guarded native
operations and sparse disc overlays without rewriting or replacing that stock
image.

## Where packages live

Two catalog roots sit beside the executable, split by who owns the files:

```text
<exe>/mods/
  bundled/     build output — the framework's mods/builtin/packages plus the
               title's mods/preloaded/packages. Every build WIPES and re-stages
               this tree, so nothing a player owns may live here.
  installed/   launcher-owned — .psxmod archives installed through the Mods
               manager. No build ever touches this tree.
  state.toml   user selection state (enabled features, option values).
```

Both roots use the same `<package-id>/<version>/manifest.toml` layout and are
scanned into one catalog, bundled first. An installed package with the same id
as a bundled one deliberately shadows it and records that it did so, so an
override is visible rather than decided by directory-iteration order.

A bundled package is not removable from the Mods page: deleting build output
would succeed and then be undone by the next build. Remove it from the title's
`mods/preloaded/packages` instead.

Only `bundled/` ships in a release. `installed/` and `state.toml` are the local
machine's and are excluded by every packager.

**Migration.** A pre-split install has one `mods/packages/` tree holding both.
The first scan moves each package the current build did not also stage into
`installed/`, drops the rest as redundant build output, and removes the old
tree — but only once every version directory has been dealt with. Anything that
could not be moved is left exactly where it is and reported.

A manifest that fails to parse is never skipped silently: it is reported by
`scan_errors()` and logged, naming the path and the reason.

## How `bundled/` gets staged (titles: read this)

**The framework owns the layout. A title declares a directory, never a path
shape.** Hand the title's catalog to `psxrecomp_add_runtime_target()`:

```cmake
set(MYGAME_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")

psxrecomp_add_runtime_target(psx-runtime
    ...
    PRELOADED_MODS_DIR "${MYGAME_PRELOADED_MODS}"
)
```

`PRELOADED_MODS_DIR` names a directory shaped like
`<dir>/packages/<package-id>/<version>/manifest.toml`, optionally with a
`README.md` beside `packages/`. On every build of that target the framework:

1. wipes `<exe-dir>/mods/bundled` (build output only — never `installed/` or
   `state.toml`),
2. removes exactly the ids it is about to stage from any pre-existing
   `mods/packages`, which migrates a build directory made before the split
   while leaving a player's own legacy packages for `migrate_legacy_root()`,
3. copies the framework's `mods/builtin/packages/<id>` then the title's
   `<dir>/packages/<id>` into `mods/bundled/<id>` — in that order, so a title
   may deliberately OVERRIDE a builtin at the same id and version (Tomba 2's
   Italian runtime ships localized `psx.*` manifests exactly this way),
4. copies `<dir>/README.md` to `mods/README.md`, and
5. verifies the result with `runtime/psx_check_mod_catalog.cmake`.

Pass `PRELOADED_MODS_DIR NONE` to declare that a target intentionally ships no
game catalog. A target built with `COSIM` stages nothing: it has no launcher,
and it shares an output directory with the real runtime.

**Do NOT write your own `copy_directory` into `<exe-dir>/mods`.** Five titles
did, and the reason it is now a build error is worth stating: a hand-written
copy names the destination as a *string*, so when framework commit `4cc04be3`
renamed the staged catalog from `mods/packages` to `mods/bundled`, all five
kept staging into a directory nothing reads. Nothing failed to configure,
compile or link — the coupling has no compile-time or link-time consumer — and
the defect surfaced only when a release packager ran, in a different
repository, on a later day. Two guards now close that window:

* **configure time** — a project with packages under
  `mods/preloaded/packages` that does not declare `PRELOADED_MODS_DIR` is a
  `FATAL_ERROR`, naming the packages and the argument to add.
* **build time** — `psx_check_mod_catalog.cmake` runs as the *last* `POST_BUILD`
  step (registered through `cmake_language(DEFER)`, so it lands after anything
  the title registered) and fails the build if a declared package did not reach
  `mods/bundled`, or if any package this build stages turned up under
  `mods/packages`. It is also registered as the ctest
  `psx_staged_mod_catalog_test`.

Both are exercised by `runtime/tests/test_mod_catalog_layout.py`.

The release packagers assert the same invariant one layer out:
`Add-ModCatalog` in `tools/release_overlay_stage.ps1` reads `mods/bundled` and
refuses to package when a package the *sources* define is missing from it. It
asserts that invariant rather than a hard-coded count, because a count
describes only one side of a catalog two repositories contribute to and goes
stale the moment either side gains a mod.

## Feature manifest

Write new manifests at the current format version, which is **6**. Older
versions stay readable so installed packages survive an update, and each
section below notes the version a field first required.

```toml
format_version = 6
id = "example.localization"
version = "1.2.0"
name = "Example Localization Pack"
author = "Example Author"
description = "Independent title and script features."
license = "MIT"
resolver = "declarative"

[[target]]
game_id = "SLUS-00000"
# Required for disc overlays. Use the digest of the supported stock image.
disc_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[feature]]
id = "title-screen"
name = "Title Screen"
description = "Selects the title-screen artwork."
group = "Localization"
default_enabled = false

[[feature]]
id = "retranslation"
name = "Retranslation"
description = "Uses the revised English script."
group = "Localization"

[[option]]
feature = "title-screen"
id = "variant"
label = "Title artwork"
type = "choice"
default = "rockman"

[[option.choice]]
value = "mega-man"
label = "Mega Man X6 (USA)"

[[option.choice]]
value = "rockman"
label = "Rockman X6 (Japan)"

[[patch]]
feature = "title-screen"
target = "main_exe"
address = 0x80041234
expected = "2a 00 02 24"
replace = "0e 00 02 24"
when = { variant = "rockman" }

[[overlay]]
feature = "retranslation"
target = "disc_raw"
offset = 123456
file = "assets/retranslated-script.bin"
sha256 = "..."
# Optional additional guard over the same range in the stock image.
expected_sha256 = "..."
```

Every `[[option]]`, `[[patch]]`, and `[[overlay]]` in a feature-style manifest
must name its owning feature. Ambiguous operations are rejected.

Option types are `boolean`, `choice`, and bounded `integer`. Conditions are
feature-local: `when = { option = "value", ... }` requires every listed option
to match. The legacy `when_option`/`when_value` pair remains accepted for a
single condition.

## Bounded integer patches

Package format 2 can encode a bounded integer option directly into a guarded
write:

```toml
format_version = 2

[[feature]]
id = "starting-lives"
name = "Starting Lives"

[[option]]
feature = "starting-lives"
id = "count"
label = "Lives"
type = "integer"
min = 0
max = 99
step = 1
default = 2

[[patch]]
feature = "starting-lives"
target = "main_exe"
address = 0x8001DE64
expected = "02 00 02 24"
replace_from = { option = "count", encoding = "u16le", offset = 0 }

[[patch]]
feature = "starting-lives"
target = "main_exe"
address = 0x8001DE70
expected = "03 00 02 24"
replace_from = { option = "count", encoding = "u16le", offset = 0, addend = 1 }
```

`replace_from` and literal `replace` are mutually exclusive. The referenced
option must be a bounded integer owned by the same feature. The initial
encodings are `u8`, `u16le`, and `u32le`. `offset` selects a byte field inside
the expected guard and defaults to zero. Generated replacement bytes begin as
an exact copy of the expected bytes, then the encoded value replaces only that
field. This lets a MIPS immediate, for example, retain a guard and collision
claim over its complete instruction. `addend` is the only supported transform,
and the complete declared option range after that addend must fit the unsigned
encoding.

There is deliberately no host-endian encoding, signed inference, mask, shift,
scale, expression language, or partial-field merge. A package uses multiple
guarded `[[patch]]` entries when the same value has multiple destinations.
Generated bytes enter the ordinary pre-boot write plan, collision checks, and
fingerprint. A generated value identical to the stock guard is omitted as a
no-op, so an enabled stock-valued option does not claim or conflict on bytes it
does not change.

Integer values use canonical decimal text. Leading plus signs, redundant
leading zeroes, values outside the bounds, and values not aligned to `step` are
rejected.

## Ordered values and split MIPS immediates

Package format 3 adds feature-local ordering constraints for related integer
fields:

```toml
format_version = 3

[[constraint]]
feature = "rank-thresholds"
kind = "ordered_integer"
direction = "nondecreasing"
options = ["rank-c", "rank-b", "rank-a"]
```

All listed options must be integer options on that feature. Defaults must
satisfy the constraint. While a feature is enabled, an edit that would invert
the order is rejected with the neighboring option labels. Disabled features
may retain an incomplete or invalid draft, but cannot be enabled until it is
valid. `nonincreasing` is also supported.

Format 3 also provides a narrow, typed transform for constants constructed by
a linked MIPS `LUI`/`ORI` pair:

```toml
replace_from = {
  option = "speed",
  encoding = "mips_lui_ori_u32",
  omit_when_default = true
}
```

The patch must target one aligned, fully guarded eight-byte `main_exe`
instruction pair. The loader verifies the opcodes and register linkage, then
places the raw high and low 16-bit halves into the two immediates. It does not
apply signed-`ADDIU` carry adjustment. `offset` and `addend` are not accepted
for this encoding.

`omit_when_default` suppresses the entire patch when the selected value equals
the option default. This models source tools whose declared default means
"make no writes," including cases where multiple guarded sites contain
different stock values. For any nondefault selection, every declared site
retains its collision claim even if one generated replacement happens to equal
its stock guard.

## Sparse fields and integer predicates

Package format 4 separates a patch's complete expected-byte guard from the
fields it owns and writes. This is for semantic records whose independently
configurable fields share one useful guard:

```toml
format_version = 4

[[option]]
feature = "saber-timing"
id = "frames"
label = "Frames"
type = "integer"
min = 0
max = 99
default = 2

# Positive values change only byte 0.
[[patch]]
feature = "saber-timing"
target = "main_exe"
address = 0x80077640
expected = "02 42 01 02"
fields = [
  { offset = 0, option = "frames", encoding = "u8" },
]
when_integer = { option = "frames", op = "gt", value = 0 }

# Zero has a narrow, explicitly declared compound representation.
[[patch]]
feature = "saber-timing"
target = "main_exe"
address = 0x80077640
expected = "02 42 01 02"
fields = [
  { offset = 0, replace = "01" },
  { offset = 2, replace = "00" },
]
when_integer = { option = "frames", op = "eq", value = 0 }
```

`fields` is mutually exclusive with `replace` and `replace_from`. Every field
has a nonnegative `offset` and exactly one payload form:

- `replace = "..."` supplies non-empty literal bytes; or
- `option` plus `encoding` uses a bounded integer option on the same feature,
  with the optional checked `addend`.

Fields must fit inside `expected` and their owned byte ranges may not overlap.
The supported dynamic encodings are the existing `u8`, `u16le`, `u32le`, and
linked `mips_lui_ori_u32` forms. A sparse MIPS field owns only the two
immediate halfwords while the complete linked instruction pair remains
guarded; as with format 3, that encoding does not accept an addend.

Resolution omits individual fields whose generated payload already equals the
guard. If no field changes, the patch is a no-op. Otherwise the plan retains
the complete guard and the exact remaining owned ranges. Runtime guard
validation checks every byte of `expected` before applying any writes, while
collision detection and writing use only the owned fields. Thus two features
can safely own adjacent bytes in one record without either overwriting the
other. Overlapping guards must still agree on their expected bytes; conflicting
guards make the plan unsatisfiable and are rejected.

`when_integer` is a patch-level, feature-local predicate over one bounded
integer option. `op` is exactly one of `eq`, `ne`, `lt`, `le`, `gt`, or `ge`,
and `value` is an integer constant inside the option bounds. Equality
constants must also be selectable under the option's `step`. String-valued
`when` conditions and one `when_integer` predicate may coexist and are ANDed.

Sparse fields and integer predicates are still pre-boot plan construction.
They do not provide a general expression evaluator, masks, arithmetic beyond
the checked field addend, package code execution, or per-frame dispatch.

## Channels

A feature declares how finished it is. Format 6 puts `channel` on the
**feature**, not the package:

```toml
format_version = 6
id = "example.enhancements"

[[feature]]
id = "widescreen"
channel = "experimental"     # ships, badged, default off

[[feature]]
id = "hook-trace"
channel = "developer"        # absent from any release build
```

| Channel | Ships | In the launcher |
|---|---|---|
| `stable` (default) | yes | no tag — the absence is the stable case |
| `experimental` | yes | amber `EXP` tag, and a line saying it is unvalidated |
| `developer` | **no** | secondary-accent `DEV` tag; only ever visible on a local build |

An absent `channel` means `stable`. A package may still declare one, which its
features inherit unless they state their own — which is how a format-5 manifest
carrying a package-level `channel` keeps working unchanged.

**Why the feature and not the package.** A package is the installation and
**trust** boundary; how finished one of its features is has nothing to do with
that. When the marker sat on the package, a catalog holding one player-ready
feature and one developer instrument had to declare itself entirely
developer — so neither shipped, and the documented workaround was to split the
catalog into two packages. Channels per feature remove that trade.

**"Developer does not ship" means absent, not hidden.** Two mechanisms, one per
catalog root:

- **`bundled/` is filtered when it is staged.** `tools/mod_channel_filter.py`
  emits a manifest without the developer features and without the `[[option]]`,
  `[[patch]]`, `[[overlay]]`, `[[plugin]]`, `[[resource]]` and `[[constraint]]`
  entries that only served them; a package whose every feature is developer has
  its directory removed. This is generation, not rewriting: the staged catalog
  is build output and the author's manifest in the repo is never touched.
- **`installed/` is refused at load.** A third-party archive is never modified,
  so the runtime declines to surface developer features from one instead.

The runtime gate is the build definition `PSX_MOD_DEVELOPER_CHANNEL`, which
`runtime.cmake` sets for a local build and clears under `$CI`. A contributor
reaches developer features by cloning the repo and building; a release build
carries neither the features nor their operations, so a stale `state.toml`
naming one cannot reach them either.

Packaging defaults the exclusion from `$CI` — on under any CI provider, off
locally. `--exclude-dev-mods` / `--include-dev-mods` (or `EXCLUDE_DEV_MODS=0|1`)
override it, and `project_studio build export --exclude-dev-mods` reproduces
what a release would contain.

## Trusted static plugins

Package format 5 can activate a game-owned plugin that is already statically
linked into the executable:

```toml
format_version = 5

[[feature]]
id = "warp-debug-menu"
name = "Warp Debug Menu"

[[plugin]]
feature = "warp-debug-menu"
id = "example.warp-debug"
```

The plugin id is a stable registry key, not a library path or symbol name. The
package archive supplies no native code. Resolution fails before launch when
an enabled plugin has no registered implementation or when two features claim
the same plugin id. Active plugin identities and owners participate in the
canonical plan fingerprint.

An implementation may register an activation callback, a deterministic
guest-VBlank callback, or both under the same id. Activation runs after the
launcher's final mod-plan commit and before renderer/window initialization; it
is appropriate for a game-owned mod that selects a fixed display aspect or
another pre-boot host feature. VBlank callbacks run from the emulated GPU
VBlank event, independent of host presentation, pacing, turbo, or skipped
frames. Trusted callbacks receive only the narrow C services exposed by
`runtime/include/mod_plugins.h`. Games should continue to use declarative
patches and overlays when those operations are sufficient.

`psx_mod_set_load_acceleration(multiplier, release_frames)` is the narrow
pre-boot service for a game-owned fast-loading feature. It changes host
wall-clock pacing only: guest VBlanks, CD deadlines, interrupts, callbacks, and
game logic still execute. Multipliers 2 through 16 are bounded choices; zero
selects uncapped host speed. A zero-frame release stops acceleration as soon as
the sustained-load predicate clears, which is appropriate for timing-sensitive
or speedrun-oriented packages.

`psx_mod_set_disc_speed(divisor, instant_max_per_frame)` is the guest-visible
alternative. Divisors 2 and 4 shorten emulated CD deadlines; zero selects the
bounded instant scheduler. Because this changes interrupt timing, packages
should label it experimental and make it mutually exclusive with host-only
load acceleration (normally as choices in one default-off feature).

## Native operations

`main_exe` writes use PSX guest virtual addresses. Expected bytes are checked
after the BIOS loads the executable, then the complete write plan is applied
before its entry point. Changed executable ranges use the existing dirty-RAM
interpreter/native-overlay machinery; untouched functions remain on the static
native path.

Small `disc_raw` and `disc_user` patches are equal-length guarded writes and may
not cross a sector boundary:

- `disc_raw` offsets use `lba * 2352 + byte_in_sector`.
- `disc_user` offsets use `lba * 2048 + byte_in_sector`.

File-backed `[[overlay]]` operations are intended for large assets and may span
any number of sectors. Their paths must remain inside the archive. Payload
size and SHA-256 are verified while scanning, but disabled payloads are not
retained in memory. Enabled payloads are loaded and reverified during
resolution, then indexed by target and LBA before boot. A CD read performs a
direct indexed lookup rather than scanning every installed mod.

Feature disc overlays require an exact `disc_sha256` on every target entry.
`expected_sha256` can additionally guard the replaced stock range.

## State and migration

`mods/state.toml` format 2 stores selected package versions separately from
per-feature enabled states and values:

```toml
format_version = 2

[[package]]
id = "example.localization"
version = "1.2.0"

[[feature]]
package_id = "example.localization"
id = "title-screen"
enabled = true

[feature.values]
variant = "rockman"

[feature.resources]
artwork = "C:/Users/You/Pictures/example-bezel.png"
```

State format 1 and package-only manifests remain readable as a migration aid.
They appear through one synthetic legacy feature. New packages should use
explicit features.

The old `derived_disc` VCDIFF mechanism is legacy conversion scaffolding only.
Feature-style manifests reject it. It is not a product mod primitive, fallback,
or image-selection workflow; patched discs may be used offline as parity
oracles while converting known mods to native operations.

## Resolution and diagnostics

Before boot, the manager:

1. verifies the selected stock game and revision;
2. expands only enabled features and their selected options;
3. orders active packages deterministically by dependencies;
4. verifies enabled payloads and operation bounds;
5. collision-checks the complete owned byte-range plan and guard
   compatibility;
6. coalesces only truly identical target/range/expected/replacement writes or
   identical overlays; and
7. produces a canonical SHA-256 plan fingerprint.

Incompatible overlaps fail before launch. Structured diagnostics identify both
`(package, feature)` owners and the exact contested target range. The launcher
marks both feature rows and lets the user decide what to disable. It never
silently chooses a winner.

Operation boundaries are not semantic boundaries. Legacy full-record writes
compose when both their expected and replacement bytes agree throughout the
owned intersection. Format-4 sparse patches collide only on declared owned
fields, while their complete guards must remain mutually compatible.
Partially overlapping overlays compose when their replacement payload bytes
agree. A differing owned byte or incompatible guard produces a diagnostic at
that exact location. Exact duplicate operations may be coalesced.

Package-level dependencies and conflicts are reserved for actual implementation
relationships. Mutually exclusive choices such as US versus Japanese artwork
belong inside one feature as option values.

## Trusted adapters and archive safety

### Owner-selected resources

Format-5 packages may declare feature-owned resources that the launcher renders
with its native file/folder picker:

```toml
[[resource]]
feature = "bezel"
id = "artwork"
label = "Bezel image"
description = "Select an image to draw behind the game frame."
format = "file"
file_patterns = "*.png,*.jpg,*.jpeg,*.bmp"
file_description = "Image files"
required = false
```

Resources are paths selected by the player and persisted in `mods/state.toml`;
they are not copied into the package. Optional resources with no selected path
are omitted from the committed plan. Required resources reject launch while the
feature is enabled and unset.

`resolver = "builtin:<id>"` selects a resolver statically registered by the
game. Format-5 plugin ids likewise select only statically registered
implementations. Packages cannot load arbitrary native code or select
arbitrary symbols.

The installer accepts stored or DEFLATE-compressed ZIP entries, validates CRCs,
rejects encrypted entries and unsafe or absolute paths, limits archives to 4096
files and 256 MiB expanded size, stages extraction, validates the manifest, and
publishes the version atomically.

### Presentation bezel packages

The framework registers `psx.bezel`, a trusted presentation plugin for OpenGL
margin artwork. The built-in package declares an optional `artwork` image
resource; when the feature is enabled and the player has selected an image, the
runtime draws that image behind the game frame before presenting the normal 4:3
or widescreen content.

The built-in package `psx.presentation.bezel` targets every game but defaults to
off, so the default presentation is unchanged: letterbox and pillarbox margins
remain black. Enabling the feature without choosing artwork is also a no-op.
The package supplies only the declaration and trusted plugin selection; archives
still cannot load native code.
