# AOT Overlay Sharding — Spike Findings & Plan (Tomba-first)

Status: ENHANCEMENT SPIKE (2026-07-17). Foundation executed and live-proven.
Author: investigation via 4 parallel agents + adversarial source verification.
Scope: can we move overlay sharding from *runtime discovery* to *build-time (AOT)*,
starting with Tomba (SCUS-94236)?

Current final-hash checkpoint (`cg5_06162507`, 2026-07-20): cross-variant hosted
interiors recover sibling-proven entries only through a unique current-byte owner
and recipient CFG block-leader proof. A clean Tomba 1 invocation published 189/189
runtime-valid pairs (`ok=189 failed=0`): 71 organic-authority shards plus 118
`hosted-v1` non-authority supplements, totaling 11,177 manifest candidates. An
unchanged second invocation published nothing (`ok=0 failed=0`) and preserved the
complete 378-file DLL/manifest inventory byte-for-byte and timestamp-for-timestamp.
Played-vault recall is 808/823 exact entry addresses (98.2%) and 793/867 exact
`(entry, code_crc)` variants (91.5%); combined overlay+BIOS interval containment is
823/823, though containment is not exact native-dispatch proof. A no-autocompile
runtime smoke reached the village with correct rendering at 60 fps, active native
dispatch, and no candidate, range-index, or lazy-manifest overflow. Two current-RAM
CRC mismatches were blocked safely and fell back instead of executing stale code.

The prior cross-title checkpoint (`cg5_7125d9b5`, 2026-07-19) remains the latest
common-hash evidence for Tomba 2, MMX6, and Ape Escape and must be regenerated under
`cg5_06162507`. At that checkpoint, bounded strong-root batching replaced the
pathological one-DLL-per-root prototype without weakening generated-C, manifest,
ABI, pair, or live-byte gates. MMX6 published 107/107 runtime-valid pairs and covered
588/701 historical observations by exact entry address (83.9%), or 595/701 with the
exact BIOS resident shard (84.9%). Its 17,506 raw, pre-dedup manifest identities
motivated the reviewed 32,768 runtime candidate ceiling with durable overflow
telemetry and fail-closed one-shot bundle suppression.

Current Tomba 2 checkpoint: generic play-free extraction emits 29 base/raw regions
plus exact adjacent-producer composites (52 capture records total). The 29 base/raw
shards and the live A01 composite audit with zero unsupported instructions or bad
targets under `cg5_d0a05489`. Direct/constant-register call discovery, raw/cross-file
base recovery, and padded-return frameless-boundary retention raised full-playthrough
vault exact-address recall from 1113/1856 (60.0%) to 1270/1856 (68.4%). Those
historical figures predate duplicate-VA CRC accounting; current reports separately
count unique entry addresses and exact `(entry, code_crc)` candidates, preserving
every observed code variant at a reused address. Exact-entry parity is intentionally
no longer treated as the uncovered-code count: the runtime vault contains
fine-grained fragments, including consecutive instruction entries, while static AOT
emits broad functions. Historical interval containment was 1811/1856 (97.6%), with
45 observed addresses outside every static interval, all in the game-independent
kernel/low-memory region. Interval containment is unverified potential, not proof
that the matching code variant can dispatch natively. Runtime static-only validation
loaded the richer MAIN shard at 60 fps with `stale_blocked=0`, no range/manifest
overflow, and byte-matching candidates. The clean static-only attract soak then loaded the
459-function A01 composite and ran the formerly ~14 fps lava demo at 65–67 fps at
BelowNormal priority, with correct rendering and no range/manifest overflow. Misses
continue to fail safely to the interpreter.

---

## 2026-07-19: strict indirect-dispatch enrichment and scoreboard contract

- A framed-root candidate is accepted only at a return-adjacent boundary with a
  bounded stack frame and an in-frame `ra` save before its first control transfer.
- Indirect switch recovery accepts only the canonical bounded form: `sltiu` guard
  plus delay-slot NOP, `sll index,2`, producer-owned `lui`+`addiu` table address,
  `addu`, `lw`, and the exact `jr`. A clobber, skipped definition, malformed or
  foreign table, or invalid target rejects the whole table. Case targets and call
  continuations remain dispatch-only interiors, never roots. Python discovery,
  C++ analysis, and code generation use the same matcher and producer bounds.
- The emitted proof metadata is diagnostic. If generated-C audit or host compilation
  rejects an enriched recipe, compilation strips framed roots, aliases, dispatch
  PCs, and proof fields, then retries the exact producer bytes with direct-JAL roots
  only.
- The scoreboard reports three distinct bounded metrics: exact entry address; exact
  `(entry, code_crc)` candidate, retaining multiple variants per address; and
  unverified interval containment potential. Vaults and capture histories are finite
  observed needed sets, never exhaustive title inventories. A reported 100% means
  only 100% of the named observed set under the named metric.

---

## 2026-07-18: byte-proven archive producers

- The generic extractor now recognizes strict 0x800-aligned `{id,size}` member
  archives (MMX6 `ROCK_X6.BIN`) and companion HED sector tables spanning DAT/BNS
  payload namespaces (Ape Escape `KKIIDDZZ.*`).
- Link bases are not guessed from title constants. Multiple independent members
  must anchor one sharp jal/prologue consensus; a sibling then needs at least two
  votes for that exact trusted base.
- Mixed archive members keep locally callable direct-JAL targets as a conservative
  retry recipe. Normal-mode entries and exact aliases are optional enrichment:
  generated-C audit or GCC rejection automatically retries the same byte-proven
  member with direct roots only. The final disposable MMX6 build compiled all 30
  members plus the BIOS resident shard (`ok=31 failed=0 skipped=0`). Across 16
  legacy capture files it contains 636/677 observed entries in overlay intervals
  (93.9%); all 41 observed addresses outside those intervals are BIOS/kernel PCs,
  and the separately recompiled base BIOS raises combined interval containment to
  677/677 (100.0%) for that observed set.
- Ape's two PS-X EXE mirrors are deduplicated in favor of the self-describing EXE
  producer. Its remaining 44 HED/BNS members plus two minigame EXEs and the BIOS
  recipe are also audit-clean in a disposable cache.
- A fresh 47/47 clean Ape rebuild then completed a title-to-attract-to-title
  soak across four overlay generations. All 72 exercised game PCs lie in static
  compiled ranges; the other 35 PCs are covered by the separately generated
  BIOS/kernel ranges, yielding 107/107 combined interval containment for that
  observed set. Live-byte guards safely rejected 21 stale candidates, visuals
  remained correct, and the dev history recorded 21 verified immutable snapshots
  with no invalid or duplicate references.
- Normal PS-X discovery now exports its exact overlapping `F/R` alias recipes.
  Clean overlay builds therefore preserve decoder-proven indirect entries without
  promoting every entry to a hard root or borrowing a previous cache manifest.
  T2 compiled 53/53 disposable candidates with zero audit failures and restored
  100% combined BIOS interval containment for both observed sets.
- Large archive reads are now linear rather than quadratic sector concatenation.
  These validations wrote only `%TEMP%` caches; title caches remain untouched.
- DLL publication is now atomic: compilers target a unique sibling temporary,
  successful non-empty output replaces the final cache name, and cache reuse
  requires its non-empty range manifest. Interrupted compiles can no longer
  poison later runs or overwrite a prior good shard during `--force`.
- Tomba 1 header exports are authoritative mid-function dispatch entries rather
  than callable roots. Their conservative retry uses the nearest preceding
  prologue host, stable-round call discovery prevents interior callees from
  truncating that host, and dense local pointer tables are rejected as code.
  Bounded return-to-return scanning also recovers unreferenced frameless leaves.
  The clean 25-record corpus produces 17 unique shards with zero audit failures.
  Overlay ranges cover 820/823 historical-vault PCs and 410/450 live-history PCs;
  every residual is in the separately recompiled BIOS/kernel ranges, yielding
  **100.0% combined interval containment** for both observed scoreboards.
- The resulting `cg5_060368b2` checkpoint was re-extracted from disc and tested
  against disposable caches on the other producers. Ape rebuilt 47/47 GCC shards
  and retained 107/107 combined live-history interval containment. MMX6 rebuilt 31/31;
  two unsafe broad-discovery attempts failed their audit/compile gates and fell
  back to conservative seeds, retaining 677/677 combined interval containment. Tomba 2
  audited all 53 candidates with zero unsupported/bad targets, TCC-built the full
  53/53 set, and GCC-built 11 representative shards including MAIN, large area
  variants, OPN/CRD, and BIOS.
- TCC overlay compilation now receives the same release/debug and cycle-model
  defines as GCC. A TCC-only generated-source shim supplies `__builtin_ctz`
  without changing the hashed runtime headers. The packaged TinyCC 0.9.27
  toolchain compiles the complete T2 corpus, and atomic publication now removes
  its staged `.def` sidecars after both success and failure.

---

## ⭐ SESSION-2 BREAKTHROUGH (2026-07-17, VERIFIED FROM DISC + CORPUS)

Adversarial disc analysis overturned the pessimistic framing below. For Tomba's
GAME-CODE overlays, **pure static analysis suffices — no playthrough, no headless
execution, no decompressor.**

Verified facts (byte-checked against `Tomba! (USA).bin` + the 99-capture vault):
- The disc carries a full ISO9660 directory: **1085 files, 20 `AREA` dirs.** Game code
  overlays are **17 distinct `X0N.BIN` files** (one per area; `X07→X01`, `X12→X04`,
  `X15` absent, `AREA19` dual) — each duplicated under `SYSTEM/`. All 17 enumerate in
  seconds via `extract_overlays.py`'s ISO9660 walker.
- **`X0N.BIN` is uncompressed and POSITION-FIXED.** `AREA00/X00.BIN` (343,864 B) and
  `X09.BIN` (299,576 B) were found **100% byte-verbatim, contiguous** inside their
  captured `0x800E7000` RAM images (X00 at region offset 904; the 904-byte prefix is
  `0x11111111` fill). The file's export pointers are pre-relocated ON DISC to the exact
  load address (`0x800E922C` = `0x800E7388 + file-offset`), so **disc bytes == RAM
  bytes at a fixed address — zero runtime relocation, zero decompression.** This is
  Class A behavior; the earlier "scatter-load / must-capture-post-fixup" concern
  applies to the `.GAM` DATA packages and capture timing, NOT the code.
- **`X0N.BIN` self-describes its entry points:** a plaintext `{count:u32,
  ptr[count]:u32}` header (X00: 29 pointers, all in-range). Function-entry SEEDS come
  free from the file — no execution needed to find them.
- **Determinism is measured, not assumed:** across the corpus, 95–98% of overlay
  function entries have a single stable code-CRC every time the address loads (Tomba
  `0x800E7000`: 782/820 = 95%; `0x00000000`: 100%). Residual mismatches fall to the
  interpreter via the CRC dispatch guard — coverage loss, never correctness loss.
- **The `0x80000000` region is BIOS-kernel code, not a game overlay** (entry offsets
  = A0/B0/C0 vectors `0xa0/0xb0/0xc0` + SIO stub `0xcf0`; identical structure across
  MMX5/MMX6/Vigilante8/Tomba — independent engines). AOT it ONCE from the BIOS,
  shared by every title.

⇒ **Revised project shape:** a build-time STATIC OVERLAY EXTRACTOR (walk disc →
enumerate 17 `X0N.BIN` → parse header for load-addr + seeds → synthesize
`overlay_captures.json` → existing `compile_overlays.py`). Headless materialization
(§5) drops to a FALLBACK for any title whose overlays turn out compressed/relocated,
not the primary path for Tomba.

Open verification before committing (do these first — see §6 revised):
- Prove all 17 `X0N.BIN` (not just X00/X09) are 100% verbatim vs their own area's
  coherent capture, at a consistent offset.
- Confirm no `X0N` has CPU-patched internal `jr` jump tables (disc≠RAM) — reconcile
  against `overlay_capture.c:187-196`. X00/X09 show none.
- Confirm `.GAM`/`.000` packages contain no executable code needing shards (they look
  like models/palettes/scripts = pure data → not compiled).
- Confirm Tomba code overlays live ONLY at `0x800E7000` (+ kernel `0x80000000`); no
  third code base.

---

## 0. TL;DR

- **The compile step is already AOT, AND the captured coverage already ships.**
  `tools/compile_overlays.py` runs fully offline; releases already bundle the
  precompiled gcc DLL cache (`package_release.ps1:223-235`,
  `COMPILING_OVERLAYS.md:23-33`). So "AOT the 99 captures" is the STATUS QUO, not a
  goal. The goal is coverage the dev playthrough never reached.
- **"Runtime-applied fixups" are deterministic, not session-varying** — proven by the
  shipped cache CRC-matching on other machines/fresh boots (§T0.5). AOT is achievable
  because the post-fixup image is a fixed function of the disc; you just have to
  *evaluate* it (run the loader), not read it off the disc.
- **The runtime-only input is the CAPTURE, not the compile.** The single artifact
  that today only exists after a played session is `overlay_captures.json` — the
  post-fixup RAM images + execution-verified entry seeds.
- **Naive "scan the disc" AOT is DISPROVEN for Tomba** (two independent reasons,
  both verified in source — see §4). Do not pursue it as the Tomba path.
- **The faithful, general AOT path is HEADLESS MATERIALIZATION:** run the game's
  *own* recompiled loader + decompressor + fixup code at build time, driven over
  every overlay, to produce the exact `overlay_captures.json` a playthrough would —
  then feed the existing offline compiler. This uses the game's real code (no
  per-game decompressor reimplementation → satisfies Rule -1), is general to every
  title, and is guarded by an existing correctness net.
- **The correctness net makes AOT safe to be aggressive.** Shards are
  content-addressed `(entry_pc, code_crc)`; at dispatch the loader hashes live RAM
  and runs native *only* if it matches (`overlay_loader.c:2137`). A wrong/stale AOT
  shard never fires — it falls to the interpreter. AOT cannot cause divergence.

---

## 1. How overlays work today (verified)

### 1.1 Detection (runtime)
- Trigger: **CD-ROM channel-3 DMA completion** into game RAM below the FMV floor.
  `dma.c:484-498` → `overlay_capture_on_dma(load_start, size, ram+load_start)`.
- Gated on `[runtime] overlay_cache` and auto-armed on the first post-BIOS-handoff
  DMA (`overlay_capture.c:82-89`). Dedup by `load_addr` (`:91-95`).
- A second signal — sustained dirty-RAM-interp pressure sampled ~every 2s while
  *not* mid-CD-load — auto-fires fresh captures for new *variants* at a reused
  address (`overlay_capture.c:299-532`).

### 1.2 Capture record (`overlay_captures.json`, schema "v2")
Built from **live-RAM dirty pages at a coherent moment**, one record per contiguous
dirty run (`overlay_capture.c:200-233`):
```
load_addr, size, bytes_b64 (LIVE post-fixup RAM),
executed_pcs[], dispatch_entry_pcs[], function_entry_pcs[], seeds[]
```
Play-free static extractors may add the optional
`static_dispatch_entry_pcs[]` field. It is a strict subset of
`dispatch_entry_pcs[]` naming entries derived by the play-free scanner or read
from an authoritative on-disc export/header/jump table. It never changes the
local dispatch set and is never
synthesized from runtime rings or prior manifests. The compiler uses this
provenance only to nominate the same address in sibling byte variants; recipient
bytes must independently prove a current CRC-valid owner, CFG block leader, and
exact emitted identity before an alias can be published.

Cross-variant compilation closes every strong exact root for every byte recipe
before freezing sibling donor/owner authority. Roots that overlap live, forced,
or interval evidence retain singleton failure isolation but still close in that
pre-freeze phase. Hosted aliases and later generic orphan fragments carry atomic
manifest provenance included in their pair and filename identities: the runtime
still loads them as ordinary CRC-guarded coverage, while later compiler runs exclude
the entire supplemental shard from authority, including incidental rooted callees.
The bounded nomination window depends only on frozen authority; supplemental cache
hits remain in that fixed universe through owner selection and every alias/host
cap, then are removed by the compile-time `still_needed` guard. This prevents
repeated invocations from paging into new targets at any later cap. Partial hosted
successes retry their fixed remainder immediately.

Offline publication is also bounded by the loader's real resource model. The
shared `PSX_OVERLAY_CANDIDATE_CAP` counts every `F` candidate from one
representative of each distinct complete generated pair per compiler tier.
Deduplication is deliberately whole-pair only: the `P` identifier, same
normalized provenance class (unmarked authority, `hosted-v1`, or `orphan-v1`),
and exact ordered, physical-address-normalized `F`/`R` semantics must all match.
Legacy/no-`P`, unknown, malformed, cross-tier, or partially different pairs
remain distinct.
The runtime fully preflights a later physical twin, closes its redundant handle
before initialization, and reuses the canonical pair's candidates and flush
owner. `overlay_loader_status.pair_aliases` counts these validated physical
twins. CRC dispatch validation is unchanged.

The other publication-gating physical lazy/cache-index resources do not
deduplicate. Every selected physical pair still contributes its raw `F` rows,
unique 4 KiB range-page links per `F`, and cache-index file. Before committing a
pair, the compiler locks the game/architecture/codegen cache namespace,
inventories the exact GCC-first runtime selection, subtracts the pair being
replaced, and independently projects four bounds: candidates at
`PSX_OVERLAY_CANDIDATE_CAP`, raw lazy `F` rows at twice that cap, lazy range-page
links at eight times the raw-row cap, and 4096 selected files. Exact cache hits
and replacements that do not grow any already-exceeded independent budget remain
legal; crossing or worsening any one bound rejects that optional coverage to the
interpreter. The loaded-candidate range index has its own bounded fallback scan
and is not part of offline publication accounting.

Near a saturated namespace, each attempted repair still performs the complete
authoritative projection. To keep that tail practical, the publisher memoizes
only successful ABI/export/manifest validation for unchanged physical pairs,
keyed by expected ABI plus replacement-sensitive DLL and manifest file identity.
Changed pairs revalidate, and negative loader results are never memoized because
they may be transient. Transaction recovery and the locked projection remain on
every publication path; the memo only avoids remapping hundreds of unchanged
DLLs for each rejected optional shard. Once generated `F/R/P` metadata proves
a shard cannot fit, the compiler applies that same locked projection before
invoking the native linker; admitted shards are still projected again after
linking at authoritative publication. A concurrent deletion can therefore only
defer optional coverage to the next invocation, never admit an over-cap pair.
An earlier over-cap snapshot may be reused only when it still proves rejection;
anything that appears admissible forces a fresh locked inventory. This avoids
even the directory scan in a saturated fixed-point tail without turning stale
state into admission authority.
Dynamic recipes are processed in a canonical order under a whole-invocation
namespace lock. This makes the accepted subset reproducible even at the hard
capacity boundary instead of allowing worker scheduling to choose the winner.
The runtime rejects legacy/no-CRC and malformed range manifests, keeping its
registration contract identical to the inventory parser.
That shared contract is deliberately narrow: ASCII records, LF/CRLF physical
lines, bounded line widths, canonical KSEG0 entries, one authoritative CRC,
1..16 in-RAM ranges containing the entry, and no duplicate physical `F` entry.
GCC/TCC tier filenames use the host filesystem's case rules. A DLL is accepted
atomically only when its ABI, optional pair id, and every manifest-declared
`func_*` export agree; a partial-export bundle registers zero candidates.

`executed_pcs`/`dispatch_entry_pcs` come from always-on interpreter ring tables
(`g_dirty_ram_pc_table`, `g_dirty_ram_exec_pc_table`) — **execution-verified**, not
disassembly guesses.

#### Durable capture history (opt-in)

`overlay_captures.json` remains the canonical latest snapshot consumed by the live
compiler, but it is published through a same-directory temporary file plus atomic
replace so readers never observe a half-written manifest. Games may additionally
set `overlay_capture_history = true` to append each changed coherent snapshot as
an independent JSONL record to `overlay_captures.addendum.jsonl` beside the
executable. Without a persist directory the v1 record embeds the full snapshot.
With dev immutable persistence enabled, a compact v2 record references the
atomically published JSON and vault merge verifies its FNV signature before use;
this avoids repeatedly embedding multi-megabyte base64 snapshots. A hard kill can
damage only the last line; the next append quarantines that tail, and
`tools/coverage_vault.py merge --addendum ...` ignores malformed records while
additively merging every valid snapshot.

Development configs may also set a project-relative
`overlay_capture_persist_dir`. This creates one atomically published, immutable
JSON snapshot per changed capture. Absolute paths and `..` components are rejected.
Production configs normally omit this key and retain only the naive executable-
local addendum. Both history modes are off by default and contain game-derived
bytes, so their outputs remain private/gitignored artifacts.
Previously embedded development histories can be converted to verified compact
references with `coverage_vault.py compact-addendum --addendum ...
--persist-dir ...`. The tool verifies every valid record against its exact
immutable snapshot before atomically replacing the addendum, and leaves the
source untouched if any snapshot is missing or mismatched.
`coverage_report.py --addendum ...` streams those verified history snapshots and
unions their dispatch/function entries into the live scoreboard, preserving
cross-launch recall without loading their game bytes into the static needed set.

The recall scoreboard can roll a persisted live gap set forward after a verified
monotonic static expansion with
`coverage_report.py --prior-report <old.json> --assume-static-superset`. The
explicit assertion is required because an old gap
manifest stores misses, not every previously covered entry; silently assuming
monotonicity would make a regressed static set look healthier than it is.

All scoreboard inputs are finite observed needed sets. Reports distinguish exact
entry-address recall, exact `(entry, code_crc)` candidate recall (including multiple
CRC variants at one address), and interval containment potential. The last metric
does not prove that the owning variant or a valid compiled interior matches live
bytes; no vault result is evidence of an exhaustive title inventory.

The base BIOS recompiler is a separate static producer. When its generated
dispatcher is available, pass `--bios-dispatch <SCPHxxxx_dispatch.c>` so the
scoreboard preserves overlay-cache-only metrics and also reports combined exact
candidate and interval metrics from the dispatch table plus guarded relocated-kernel
body ranges. Do not
manufacture overlay shards merely to duplicate those already-native BIOS bodies.

Boot-installed RAM helpers that are not contiguous ROM relocations use a separate
exact-hash recipe (`tools/aot_overlay_spike/bios_resident_code.json`). The generic
extractor emits only the listed code words/entries when the supplied BIOS SHA-256
matches. `compile_overlays.py` adds a `.resident` sidecar, and the runtime preloads
only explicitly marked resident shards. Preload is registration, not trust: every
entry still passes the same live code-range CRC before native execution. The
current SCPH-1001 recipe is exactly `0x8000DF80-0x8000DFF0`; it deliberately omits
page fill and the `0x8000DFFC` callback-pointer data. This closes the six durable
Tomba 2 kernel gaps without turning adjacent zeros/data into code.
Release extraction passes an explicit ROM with `--require-bios-resident --bios`;
missing or unsupported hashes then fail instead of silently reducing coverage.

### 1.3 Compile (offline, `tools/compile_overlays.py`)
1. Python does function-boundary discovery from the seeds (`classify_overlay_seeds`,
   `_walk_overlay_function`) with a strict callable-prologue gate; interior/
   jump-table PCs become aliases, never walk roots.
   Optional enrichment admits only return-adjacent framed roots and the canonical,
   producer-bounded switch sequence described above. Its proof fields are persisted
   for diagnosis, not treated as runtime trust.
2. Captured bytes wrapped in a **synthetic PS-X EXE** (`make_psxexe`, `:266-274`) and
   handed to the *same* recompiler binary the main EXE uses:
   `psxrecomp-game <fake.psx> --seeds seeds.txt --overlay --ws-config game.toml`.
3. Out-of-overlay calls rewritten to `call_by_address()` (resolved at runtime via the
   dispatch table); jump tables → native `switch` + `call_by_address` default.
   `audit_generated_c()` hard-rejects a shard if any direct call escapes or any
   instruction is unsupported.
4. Per-function identity = CRC32 over its exact compiled code ranges. Output:
   `<phys>_<region_crc>.dll` + a `.ranges` manifest (`F <entry> <code_crc>` /
   `R <entry> <len>`).
   Rebuilding identical region bytes also imports prior overlapping alias groups
   as non-root `retained_alias` bodies. This makes discovery additive: newly
   proven frameless roots cannot hard-cap and erase an older indirect-entry body.
   A reachable direct branch that crosses a sibling-entry hard cap may add its
   target as a static root only when a bounded target-local CFG reaches a return
   without invalid words or sequential escape.
   If audit or host compilation rejects enrichment, the tool retries the same bytes
   after removing framed roots, aliases, dispatch PCs, and proof fields; only
   direct-JAL roots remain.
   Exact-hash BIOS resident captures additionally receive a `.resident` sidecar.
   Resident evidence is monotonic for an identical canonical region identity:
   ordinary concurrent writers cannot erase it, and a later resident pass
   repairs a crash between pair publication and sidecar publication.
   Strong play-free entries that the shared partition cannot publish are retried
   only as isolated `dispatch_root` fragments. Normal-mode function-pointer
   targets require an exact F entry; current-byte alias recipes are retried only
   when outside every guarded interval in that region variant. Static-only
   deterministic audit rejects publish nothing and become memoized skips, while
   executed or operator-forced rejects remain hard failures.
5. `--static` mode instead namespaces the C and folds variants into
   `overlays_static.c` linked directly into the binary, dispatched via a generated
   `psx_overlay_dispatch()` — **this is already an AOT delivery path**; it moves
   *link time* earlier, not *discovery time*.

### 1.4 Dispatch + cache identity (`overlay_loader.c`)
- Two stacked cache tags: ABI (`PSX_OVERLAY_ABI_VERSION`=13 + flavor) and codegen
  (`cg<VER>_<HASH>` path segment, auto-invalidated on any emitter source change).
  Plus `verify_recompiler_matches_tag()` staleness guard.
- **Content guard (the safety net):** each DLL export registers a `Candidate` with
  `crc_code`. On dispatch, `range_candidate_matches` re-hashes live RAM and runs
  native only if `live == crc_code` (`overlay_loader.c:2137`; registration gate at
  `:595`). Cache is **additive** — every variant ever seen for an address stays
  loaded; per-dispatch CRC picks the one that currently matches (`:1844`). This is
  how one address (`0x800E7xxx`) hosts village vs overworld safely.
- Marked BIOS-resident shards are registered during cache scan because their exact
  compact code envelope is not the runtime's page-coalesced kernel region key.
  They begin invalid while boot RAM differs, then revalidate after the BIOS writes
  the expected bytes. Unmarked shards retain lazy region loading.

---

## 2. Tomba specifics (verified)

- Main EXE: `load_address=0x80010000`, `entry=0x8006B58C`, `text_size=0x88000`,
  loads contiguously, **no kernel-style relocation** (`game.toml:8, 276-285`).
- **Only two overlay load addresses** across 99 captures in
  `_coverage_vault/SCUS-94236/`:
  - `0x80000000` — 45 caps, 52–60 KB.  → 11 compiled DLL variants.
  - `0x800E7000` — 54 caps, 16 KB–624 KB (reused per scene). → 18 variants
    (4 ranges-only / audit-failed).
  - ~5.3 MB total DLL payload; largest DLL 879 KB.
- Loader = **`FUN_80021340`** (asset/area loader AND live world-streamer; green
  thread, 1 state-step per vblank via `FUN_800171D4`→`ChangeTh`).
- Decompressor = **`FUN_8003EF50`** — named in `game.toml:290` as "Tomba's real
  back-reference decompressor" (currently quarantined from the *data-shard replay*
  path; irrelevant to AOT, which runs it rather than replaying it). Exact algorithm
  not yet written up, but the function is identified and is already statically
  recompiled into the main EXE.
- On-disc format = **scatter-load**: each sector is `(12-byte descriptor,
  2048-byte block)`, descriptor names the dest RAM address; content is *also*
  decompressed and *also* CPU-fixed-up after landing.

---

## 3. The four runtime-only gaps (what AOT must supply)

From the runtime-arch investigation, the inputs the runtime path has that a static
pass lacks:
1. **Enumeration** of every distinct overlay *image* per RAM window (keyed by
   content, not address).
2. **Load address** each image lands at. *Partially static already:* the region
   floor is `(load_address+text_size)&0x1FFFFFFF` from the EXE header
   (`main.cpp:3229`); base addresses (`0x80000000`, `0x800E7000`) are known.
3. **The post-fixup byte image** — decompressed AND relocated AND jump-table-patched
   by the game's own code. *This is the crux for Tomba.*
4. **Execution-verified entry seeds** (`executed_pcs`/`dispatch_entry_pcs`).

Gaps 3 and 4 are why capture is runtime-only today.

---

## 4. Adversarial findings (what NOT to do, and why)

**F1 — Naive disc-scan AOT is dead for Tomba (two independent kills):**
- (a) Scatter-load means the reconstituted RAM image is **not contiguous anywhere on
  disc** (`docs/overlay-discovery.md:36-45`). A CRC/byte scan can't find it.
- (b) Even after de-scattering + decompressing, the executable bytes differ from the
  raw payload because the game applies **load-time fixups (relocated jump tables)**.
  Capturing pre-fixup DMA bytes was tried and caused the **village→overworld blue
  screen** — abandoned (`overlay_capture.c:187-196`). The correct image exists only
  *after the game's own code runs*.

**F2 — Two PSX overlay archetypes; Tomba is the hard one.**
- *Class A (clean Psy-Q `over(group)`):* absolutely linked, position-fixed, **disc
  bytes == RAM bytes byte-for-byte, only `.bss` differs** (Psy-Q '96 training deck;
  Legaia RE static/dynamic byte-diff). Pure-static AOT is trivially correct here
  *once the load address is known*.
- *Class B (scatter + decompress + runtime-relocate):* Tomba. Legaia's own RE team
  carved these out as **"ineligible — remain on the dynamic-capture path"**
  (`legend-of-legaia-re/docs/tooling/static-overlay-pipeline.md`). The clean-decomp
  world already concluded you must run the game for this class.

**F3 — There is no SDK-standard on-disc overlay directory.** The Psy-Q SDK ships no
mandated `{id→LBA→size→addr}` table; the loader table is bespoke per game
(1996 Sony seminar). So "enumerate overlays from a universal disc format" is a
fiction — enumeration is per-game (loader-table RE) or must ride the game's loader.

**F4 — The CRC dispatch guard makes AOT risk-free to attempt.** Because a shard only
runs on a live-RAM CRC match, AOT output is *self-validating*. Worst case of a bad
AOT shard = it never fires. This means we can ship speculative AOT coverage without
gating correctness on it.

**Confidence caveats:** the `DaCodeChick/Legaia-decomp` splat repo 404'd; PSX-splat
`.yaml` field specifics are search-derived, not re-fetched. Core Class-A claims
(position-fixed, no relocation, shared-address `over(group)`) are solidly sourced
from the Psy-Q training PDF + Legaia RE docs. Tomba compression *algorithm* is
un-characterized (function identified, bytes not decoded).

---

## 5. The strategy: HEADLESS MATERIALIZATION (the complete option)

Run the game's own recompiled loader/decompressor/fixup code at build time, over
every overlay, to produce `overlay_captures.json` **without a human playing** — then
feed the existing offline compiler. Rationale:
- **Faithful:** reproduces post-fixup bytes by executing the real code, not a
  reimplemented decompressor. No per-game byte-format hack (Rule -1 compliant).
- **General:** works for any title whose loader is recompiled (all of them),
  Class A and Class B alike.
- **Reuses ~everything:** CD-DMA sim, dirty-RAM interp, decompressor-as-code, capture
  JSON writer, and the whole compile/dispatch pipeline already exist. The new code is
  a *driver*, not a new engine.
- **Safe:** output is CRC-guarded at dispatch (F4).

Enumeration has two escalating sub-strategies (do both; (b) is the north star):
- **(a) Route-replay bootstrap:** replay a recorded set of scene/LBA load requests
  (we already have `warm_cd_routes` + the B-2 `overlay_map.jsonl` LBA provenance) to
  re-trigger each load headlessly and deterministically.
- **(b) Directory-driven completeness:** locate the loader's scene/overlay table
  statically (what `FUN_80021340` iterates), enumerate *all* scene IDs, and invoke
  the loader for each — covering scenes no playthrough visited.

---

## 6. Tiered plan (deliverables + proof artifacts per Rule 10)

### T0 — Baseline: baking the captured coverage ALREADY SHIPS (not a new task)
Correction (surfaced by user, verified): the release already bakes the captured
coverage. `TombaRecomp/tools/package_release.ps1:223-235` bundles
`build-stable/cache/SCUS-94236/…/*.dll + *.ranges` into the shipped `cache/`,
cg-tag-namespaced, loaded from first launch (`docs/COMPILING_OVERLAYS.md:23-33`:
"ship that `cache/` folder in the release"). A tcc fallback toolchain covers gaps a
player reaches past the bundle. **So "AOT the 99" is the status quo, delivered as
precompiled gcc DLLs — no user-machine recompile for covered areas.**
- The only remaining T0-flavored option is a *delivery-form* change:
  `compile_overlays.py --static` → `generated/overlays_static.c` compiled INTO the
  exe (no `cache/` dir, no LoadLibrary). Same 99, single binary. Marginal; do only if
  a single-file exe is wanted. **Not a coverage win.**
- **The real prize is coverage BEYOND the 99** — everything a dev's playthrough never
  reached. That is T1/T2, and it is the actual point of this spike.

### T0.5 — Empirical basis: post-fixup bytes are deterministic (already proven)
The shipped DLL cache CRC-matches and runs natively on other machines / fresh boots.
The dispatch guard runs a shard only if `live_RAM_CRC == compiled_code_CRC`
(`overlay_loader.c:2137`). A reusable cross-machine cache is therefore *proof* the
post-fixup RAM image is a fixed, reproducible function of the disc — "runtime-applied
fixups" are deterministic, not session-varying. AOT = evaluate that function once at
build time (run the game's own loader headless), not read raw disc bytes. Residual
genuinely-variable-immediate overlays (if any) just fail the CRC and fall to interp —
coverage loss, never correctness loss.

### T1 — Headless materialization harness (the core deliverable)
A build-time mode of the runtime that boots (HLE boot-skip), then drives the loader
directly instead of via SDL input, dumping `overlay_captures.json`.
- **T1.1 Loader ABI RE:** determine `FUN_80021340`'s inputs (scene/area id, dest
  addr, disc LBA) and the "load complete" signal (`DAT_1f8001ce`). Artifact: an
  annotated calling-convention note + a single headless call that loads ONE known
  scene and dumps a capture **byte-identical** to the vault's capture for that scene.
- **T1.2 Route-replay driver:** feed the known route (village, overworld, title, +
  `warm_cd_routes` LBAs); headlessly regenerate the current 99-capture set.
  Artifact: diff of headless captures vs `_coverage_vault` captures = empty (or only
  benign `.bss`/timing deltas). This *reproduces today's coverage with no human*.
- **T1.3 Coherence + variant convergence:** reuse the existing "coherent moment"
  capture logic so multiplexed `0x800E7000` variants are each snapshotted cleanly.
- **Proof:** `tomba-materialize.exe --captures out.json` produces a superset-or-equal
  of the vault; `compile_overlays.py` over it yields shards that all pass audit and
  all CRC-match at runtime.
- **Risk:** medium. The loader is *also* the world-streamer (the instant-loads
  parking lesson) — but here we are NOT skipping yields; we let it run to completion
  headlessly. Watch for green-thread/IRQ dependencies; drive via the real scheduler.

### T2 — Static enumeration (directory discovery → completeness)
Remove dependence on a recorded route.
- RE the scene/overlay table `FUN_80021340` consumes (the loader's own directory).
  Enumerate every scene id + its disc LBA/dest/size statically.
- Drive T1's harness over the *full* enumeration → capture overlays a playthrough
  never reached.
- **Proof:** capture count and unique `code_crc` set strictly exceed the playthrough
  vault; spot-navigate the game to 3 previously-uncaptured scenes and confirm their
  shards CRC-match (native path taken on first entry, no compile).
- **Artifact:** `game.toml [[overlays]]` populated automatically from the enumerated
  table (finally implementing the documented-but-empty schema; note the current
  discrepancy in `overlay-discovery.md` that `[[overlays]]` is unimplemented).

### T3 — Generalize the framework (Class A static fast-path + other titles)
- For Class-A titles (position-fixed, disc==RAM), add a pure-static extractor
  (extend `extract_overlays.py`, seeded by the loader table) that skips headless
  execution entirely — validated by the same CRC guard.
- ~~Fold Legaia-style `jal`-call-graph base recovery for titles lacking a clean
  table.~~ **DONE 2026-07-17** — `extract_generic.py recover_base()`: `jal`/`j`
  targets are base-independent, so at the true base the max number of intra-overlay
  jal targets land on a 0x27BD prologue. Byte-identical to the live-proven Tomba 1
  hand tool (all 22 overlays, base 0x800E7388); Tomba 2's 21 A*.BIN converge on
  0x80108F9C. Emitted page-aligned (+FILL) to match the runtime's page-aligned
  region_start key. See `tools/aot_overlay_spike/README.md`.
- Run the harness across the ecosystem (Ape, MMX, Tomba2, …); each title's shards
  become build artifacts, killing the runtime background-compile requirement fleet-
  wide.
- **Proof:** ≥2 additional titles ship with fully-baked overlays, no runtime
  compile, playthrough-validated.

---

## 7. Immediate next actions (to power through)
1. **T0 now:** `--static` bake of the Tomba vault; boot-validate; screenshot series.
   This de-risks delivery and gives a shippable artifact this session.
2. **T1.1 next:** RE `FUN_80021340` inputs; write the one-scene headless load that
   reproduces a vault capture byte-for-byte. That single artifact proves the whole
   headless thesis.
3. Then T1.2 route-replay, then T2 enumeration.

## 8. Open questions / to verify before committing effort
- Does `FUN_80021340` run cleanly to completion headlessly without SDL/vblank pacing
  it doesn't get? (Instant-loads parking says don't skip yields — we won't; question
  is whether the harness supplies enough scheduler/IRQ progress.)
- Is a scene/overlay directory table locatable statically, or must T2 fall back to
  route-replay only? (T1 works regardless; T2 completeness depends on this.)
- `.bss` / uninitialized-tail handling: captures may include zero-fill runs; confirm
  the harness snapshots at the same coherent point the runtime does.
- cg-tag/ABI: any AOT run must use a same-tree recompiler (`--codegen-hash` guard),
  and every fw bump re-rolls the tag → re-materialize. Bake this into the build.
