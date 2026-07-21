# AOT Overlay Sharding — Spike Tooling (WIP)

Play-free static overlay extraction: discover + compile overlay shards from the
disc image at build time, so a fresh install ships with overlay coverage instead
of relying entirely on runtime (tcc/gcc) autocompile. See `docs/AOT_OVERLAY_PLAN.md`
for the full design, findings, and honest coverage numbers.

**Status: ENHANCEMENT SPIKE. Provably-correct for clean producers, not exhaustive.**
`extract_generic.py` is path-parameterized and suitable for the current build
pipeline. Disc-container discovery remains engine-specific; unsupported producers
fail closed instead of being guessed as code.

Coverage reports compare against finite played/live observed sets, not exhaustive
title inventories. A reported 100% means only 100% of that named observed set under
the named metric.

## What each does

- `overlay_determinism.py` — corpus analysis across all games' captured overlays:
  base-address regularity, size/sector alignment, per-function code-CRC stability.
  Establishes that overlay code is deterministic (95-98% stable code-CRC).
- `tomba1_extract.py` — Tomba 1 (SCUS-94236). Enumerates ALL code overlays from
  the disc by header signature ({count:u32, ptr[count]:u32}), NOT just X*.BIN
  (that filter missed INFO.BIN → Dwarf Village ran interpreted/slow). Emits a
  synthetic overlay_captures.json for compile_overlays.py. Position-fixed,
  verbatim, seeds via prologue scan @ region+904.
- `tomba2_extract.py` — Tomba 2 (SCUS-94454). Two producers: MAIN.EXE (a PS-X EXE
  whose header self-describes the load address — ROBUST) and BIN/A*.BIN (header
  table). Superseded for base recovery by `extract_generic.py`'s jal-fit method.

## Validated result (see plan doc for detail)

- **Tomba 1:** 90% of played functions reproduced play-free + ~1964 unplayed
  extra; live-validated in-game (Dwarf Village went native, no runtime compile).
- **Tomba 2:** the clean play-free cache now covers 1264/1856 played entries
  (**68.1%**) and byte-matches 1144 (**61.6% entry+code_crc**). Direct-call
  provenance for frameless leaves added 74 MAIN.EXE hits over the 60.0% baseline;
  recovering the four call-sparse files added another 32 overlay-window hits;
  strict raw-code recovery of CRD/SOP added 43 more played hits, and the first
  audited adjacent-envelope shard added 2 more.
  All 22 A*.BIN overlays converge on 0x80108F9C (region 0x80108000 +3996),
  DEMO/GAME converge on 0x80106228, and OPN resolves to 0x8018A000.

## Correctness guarantee (why partial coverage is safe)

Every shard is content-addressed by per-function `code_crc`; the runtime loader
only executes a shard when live RAM byte-matches (`overlay_loader.c` lazy_man_matches).
A mis-positioned / data-as-code shard either fails audit at compile time or never
fires at runtime → coverage loss, never incorrect execution. Whatever static
misses, production autocompile (tcc/gcc) self-heals on first visit.

## Durable runtime capture history

The canonical `overlay_captures.json` is atomically replaced for live consumers.
With `[runtime] overlay_capture_history = true`, each changed coherent snapshot is
also represented by an independent record in `overlay_captures.addendum.jsonl`
beside the executable. Production's simple mode embeds the full snapshot (v1).
When an immutable persist directory is configured, dev mode writes a small v2
record referencing the already-atomically-published snapshot instead of duplicating
megabytes of base64 every autocap interval. Vault merge verifies the referenced
file's FNV signature before ingesting it. A torn final line does not invalidate
earlier or later records. Merge it into the private additive vault with:

```sh
python tools/coverage_vault.py merge --vault <vault-dir> \
  --addendum <exe-dir>/overlay_captures.addendum.jsonl
```

Dev configs can additionally set the project-relative
`overlay_capture_persist_dir` to retain immutable per-snapshot JSON files. Both
settings are opt-in; capture artifacts contain game bytes and must stay ignored.
Legacy dev addenda that embedded full snapshots before v2 can be compacted only
after every immutable copy passes its signature check:

```sh
python tools/coverage_vault.py compact-addendum \
  --addendum <exe-dir>/overlay_captures.addendum.jsonl \
  --persist-dir <immutable-snapshot-dir>
```

The replacement is atomic and aborts without changing the addendum if any valid
record lacks its exact immutable snapshot.

For a live-recall scoreboard that survives individual launches, pass the same
history to `coverage_report.py --addendum ...`. It unions only dispatch/function
entries from verified snapshots, so the report no longer depends on which latest
capture happened to be present when it ran. Runtime snapshots are cumulative
within a session. For well-formed v2 records the reporter verifies the newest
snapshot, parses the older entry lists, and skips their expensive FNV passes only
after proving every older set is contained by the verified head. The report calls
these records out explicitly as superseded but not FNV-verified; only the verified
head contributes entries. Every candidate must still carry a syntactically valid
FNV signature, and unverified parsing has per-file and aggregate size bounds.
Non-cumulative
content and records with missing, duplicate, or nonmonotonic session metadata are
conservatively processed independently with FNV verification; invalid records are
rejected.
For existing corpora that predate the addendum, `--captures` is repeatable; all
named snapshots are unioned into one live-history denominator.

PS-X EXE extraction records normal-mode discovery provenance separately from
generic function-pointer candidates. `compile_overlays.py` promotes only those
entries that also pass its local callable/CFG proof to trusted `call_root`s.
Generic static records also preserve scanner and authoritative
export/header/jump-table targets in optional `static_dispatch_entry_pcs`. The field is always a subset of
`dispatch_entry_pcs`; runtime captures and prior-manifest enrichment never invent
it. Callable members retain static provenance for sibling-variant nomination,
while case labels and mid-function members remain non-root interior aliases.
When rebuilding identical bytes, prior overlapping alias groups are retained as
non-root bodies, so stronger new roots add coverage without displacing already
compiled indirect entries.

Shard publication counts every `F` candidate from one representative of each
distinct complete generated pair inside one compiler tier. A pair must have the
same `P` identifier, the same normalized provenance class (unmarked authority,
`hosted-v1`, or `orphan-v1`), and exact ordered physical-address-normalized `F`/`R`
semantics. Legacy/no-`P`, unknown, malformed, cross-tier, or partially different
pairs remain distinct. The runtime fully preflights each later physical twin, then
closes its redundant handle before initialization and reuses the canonical
pair's candidates and cycle-flush owner. The runtime exposes the number of
validated physical twins as `overlay_loader_status.pair_aliases`. CRC dispatch
validation is unchanged.

Raw manifest rows, per-`F` unique 4 KiB range-page links, and selected physical
cache files do not deduplicate. A shared cache-namespace lock makes the projected
`existing - replaced + staged` usage atomic across concurrent GCC/TCC publishers
for all four independent bounds: candidates at
`PSX_OVERLAY_CANDIDATE_CAP`, raw lazy manifest rows at twice that cap, lazy
range-page links at eight times the raw-row cap, and the 4096-file cache index.
A replacement is legal in an already-over-budget namespace only when it does not
grow that budget and crosses no other bound. Capacity rejection is a safe
interpreter fallback and never overwrites the canonical pair.
Repeated near-cap projections memoize only successful validation of unchanged
physical pairs, using the expected ABI and replacement-sensitive identities of
both the DLL and manifest. A changed pair revalidates, while negative DLL-loader
results always retry because they may be transient. Transaction recovery and the
locked authoritative projection still run for every publication. Generated
manifest metadata also gets a conservative locked preflight before GCC/TCC, so
an already-impossible near-cap repair is rejected without paying native-link
cost; every admitted result is reprojected after linking before publication.
An over-cap snapshot is also retained as a rejection-only witness: a possible
admission always rescans under lock, while stale deletion can at worst postpone
optional coverage to the next command.
`runtime/tests/test_overlay_pair_dedup_runtime.py` compiles the real loader at a
four-slot test cap plus real shared-library fixtures, and behaviorally covers an
exact alias at capacity, handle/init/flush ownership, staged rescan idempotence,
manifest/provenance/tier negatives, and partial-export non-authority.
Dynamic compilation serializes canonically sorted recipes under a whole-command
namespace lock so a full cache has the same accepted subset on every clean run.
GCC-first basename shadowing matches the loader, and non-growing replacements
remain legal for repair. Legacy/no-CRC or malformed manifests register no native
candidates. Runtime and compiler share a strict ASCII LF/CRLF grammar, line and
range bounds, KSEG0 entry canonicalization, and platform-correct basename casing.
ABI/pair validation and every manifest-declared `func_*` export are preflighted
before registration, so a mismatched or partial DLL consumes zero candidate slots.
Reachable direct branches that cross a sibling-entry hard cap are also promoted
only after the target passes the same bounded CFG proof. This recovers out-of-line
switch/state blocks without a blind byte or pointer sweep.

## Exact-hash BIOS resident code

The base BIOS emitter covers ordinary ROM-resident code and the relocated kernel,
but SCPH-1001 also assembles a small device helper directly into RAM at
`0x8000DF80`. It is not one contiguous ROM relocation. The bundled
`bios_resident_code.json` records the exact 112-byte installed code image and its
six observed dispatch entries for BIOS SHA-256
`71af94d1...f1e99d3`. Evidence is invariant across 415 snapshots from 10 sessions.

`extract_generic.py` automatically appends the matching recipe when the framework
BIOS exists (or `--bios` / `PSXRECOMP_BIOS_ROM` names it). A different/missing BIOS
emits no resident record in diagnostic mode. Release extraction should pass an
explicit `--bios` together with `--require-bios-resident`; it exits nonzero when
the file is missing or its exact hash has no recipe. `--only-bios-resident`
materializes just these shared recipes without scanning a game disc. The capture
contains the code bytes only:
no page padding, no adjacent callback-pointer data, and no synthetic execution
claims. Its sole producer range is `DF80-DFF0`.

`compile_overlays.py` marks only manifest-produced resident shards with a
`.resident` sidecar. The runtime preloads those marked DLLs because their compact
synthetic envelope cannot equal the kernel dirty-run key. Preload only registers
candidates: normal per-function live-byte CRC checks remain authoritative. At
boot the candidate initially mismatches and is invalid; after the BIOS installs
the helper, page-generation invalidation rechecks it and enables native dispatch.
An exact T2 static-only attract soak verified all six entries native, zero
interpreter appearances, zero aborts/guard yields, correct scene transitions, and
60-61 fps at BelowNormal priority. The shard emits exactly six functions with
zero unsupported instructions or bad targets; a rejected earlier page-envelope
prototype exposed padding as a seventh function and was not retained.

## Multi-game sweep findings (2026-07-17)

`extract_generic.py` runs the improved extraction from a game.toml. Results:

- **Full-discovery for PS-X-EXE producers WORKS & is validated.** Running the
  recompiler in NORMAL mode (jr-$ra boundary scan) finds frameless functions
  overlay-mode's prologue-scan misses. Tomba2 MAIN.EXE: prologue 710 fns / 62%
  vault -> full-discovery 786 fns / 69% vault (3296 shards, 0 audit fails);
  the current tool emits 3503 discovered entries vs 1210 prologues. It also
  carries normal mode's exact overlapping-range recipes into clean overlay
  builds, so alias coverage does not depend on an older cache manifest.
- **Generality is ENGINE-SPECIFIC, not universal.** The initial two producer types
  (self-describing PS-X-EXE + {count,ptr[]} header-table) cover the Whoopee Camp
  engine (Tomba 1 header-table; Tomba 2 MAIN.EXE + BIN) but detect ZERO overlays
  on Vigilante 8 (Luxoflux) and initially detected zero on Mega Man X6 (Capcom).
  The indexed/HED archive producers below now cover MMX6 and Ape Escape; other
  engine families still need their own byte-proven container parser. Matches the
  Legaia-decomp finding: overlay enumeration is bespoke per engine.
- **SOLVED (2026-07-17) — header-table base recovery via jal-target self-consistency.**
  The old delta-sweep matched the export-table pointers against prologues, but those
  pointers point at mid-function DISPATCH entries, not prologues (measured: 0/29 hit
  a prologue at the true base) — so it scattered by 2-16KB. Root cause & fix:
  `j`/`jal` targets are BASE-INDEPENDENT (`0x80000000 | (imm26<<2)`, since all game
  code is in 0x800xxxxx). At the TRUE base, the maximum number of intra-overlay jal
  targets land on a real 0x27BD prologue. `recover_base()` votes every (jal-target,
  prologue-offset) pair for base `t-o` inside the pointer-containment window; the
  peak is unique and sharp (~2x margin over runner-up). Then the record is emitted
  PAGE-ALIGNED with a FILL prefix (page_base + (file_base - page_base)), because the
  runtime keys shards by a page-aligned region_start and compile_overlays takes
  load_addr verbatim (`phys = load_addr & 0x1FFFFFFF`, no re-align).
  - **Validation:** generic output is byte-identical (base, region envelope, seed
    set) to the live-proven `tomba1_extract.py` hand tool for ALL 22 Tomba 1
    overlays — recovered 0x800E7388 (region 0x800E7000 +904) with no hardcoded
    constants. Tomba 2's 21 A*.BIN overlays converge on 0x80108F9C (region
    0x80108000 +3996). Weak-signal files are skipped (score<4 or <1.5x runner-up),
    which is safe: coverage loss, never wrong execution, per the CRC guard.
  - The hand tools (tomba1/tomba2_extract.py) remain only as the offline oracle for
    this validation; `extract_generic.py` is now the general path.
- Bugs fixed by the sweep: multi-.bin cue (data-track selection), base-EXE
  exclusion, game.toml UTF-8 BOM, normal-mode .ranges format (`F <entry>` w/o crc).

## Raw-code and adjacent-envelope recovery (2026-07-18)

Not every position-fixed code file has a pure `{count, ptr[]}` export header.
`recover_raw_base()` applies a stricter unbounded jal/prologue vote: score >= 8,
at least 2x the runner-up, and at least 25% of apparent prologues agreeing on one
RAM-fitting base. This recovers Tomba 2 `CRD.BIN` at `0x8018A000` (32:3) and
`SOP.BIN` at `0x80108F9C` (12:3). Both are byte-identical to multiple played-vault
images; their two shards compile with zero unsupported instructions or bad
targets. It also finds Tomba 1 `OPTSUB00.BIN` (exact played-vault match) and
`DSPSUB2.BIN` (99.96% live-byte match, consistent with runtime fixups), without
touching the Tomba 1 cache.

Runtime dirty-page capture may coalesce directly adjacent files under the earlier
page key. The extractor therefore also emits exact two-producer concatenations.
For Tomba 2, `GAME.BIN` ends exactly where every A*/SOP file begins, producing
`0x80106000` envelope variants in addition to the standalone `0x80108000` shards.
The A01 composite—the live attract scene's measured hot variant—audits cleanly
(459 functions, zero unsupported/bad targets). A clean static-only soak loaded
that exact `00106000_0DC32D96` shard and ran the formerly ~14 fps lava-area attract
demo at 65–67 fps while the runtime process remained BelowNormal. The scene rendered
correctly, with no range-index or lazy-manifest overflow.

`coverage_report.py` reports three separate bounded metrics: exact entry address;
exact `(entry, code_crc)` candidate, preserving every observed CRC variant at a
reused address; and unverified interval containment potential. The distinction
matters: runtime/autocompile vaults may contain one-entry fragments at consecutive
instructions, while static AOT emits one broad function with the same PCs in its
guarded `R` ranges. Interval containment does not prove that the owning variant or
a valid compiled interior matches live bytes. At the historical Tomba 2 checkpoint,
the play-free overlay cache plus exact-hash resident shard contained 97.6% of the
observed vault addresses in static intervals. Pass the generated base BIOS
dispatcher with `--bios-dispatch generated/SCPH1001_dispatch.c` to report separate
combined metrics. Including it reached 100% interval containment for the named
finite full-playthrough and verified live-history sets; that is not exhaustive
title coverage or proof of native dispatch. This avoids generating duplicate
overlay shards for code already provided by the BIOS producer.

## Indexed and companion archive recovery (2026-07-18)

Large-container extraction first exposed an unrelated quadratic I/O bug:
`DiscReader.read_file_bytes()` appended immutable bytes once per 2 KiB sector.
It now joins sector chunks once; reading MMX6's 50.9 MB DAT fell from minutes to
about 0.25 seconds on the validation machine.

`ROCK_X6.BIN` is a strict 0x800-aligned `{id,size}[]` archive, not compressed
code. Its members independently vote three link bases (`0x800E9060`,
`0x800F9800`, `0x801E9800`). A base is trusted only after at least two members
produce a sharp >=8-vote, >=2x jal-to-prologue peak; siblings need at least two
votes for that exact consensus. Mixed archive members retain **direct-JAL roots**
as their conservative recipe. Normal-mode discovery may additionally contribute
decoder-classified entries and exact overlapping alias ranges. This enrichment
is explicitly optional: `compile_overlays.py` attempts it first, but any
generated-C audit or host-compiler rejection retries the exact same bytes with
direct-call roots only. A rejected recipe never contributes a DLL or a failure
to the final cache.

The optional indirect-dispatch enrichment is deliberately narrow. A framed root
must be adjacent to a prior `jr ra`, establish a bounded stack frame, and save `ra`
in-frame before its first control transfer. A switch must match the canonical
producer-bounded `sltiu` guard + NOP, `sll`, `lui`+`addiu`, `addu`, `lw`, exact
`jr` sequence; clobbered or skipped definitions, malformed/foreign tables, and
invalid targets reject the whole table. Case targets and call continuations are
dispatch-only interiors, never roots. Python discovery, C++ analysis, and code
generation share those bounds. Proof metadata is diagnostic; an audit or compiler
failure strips framed roots, aliases, dispatch PCs, and proof fields before retrying
the exact bytes with direct-JAL roots only.

The validation compile produced `PSX_SHARD_RESULT ok=31 failed=0 skipped=0`
(30 members + BIOS resident); unsafe enrichment fell back while clean members
retained broad ranges. Against the union of 16 legacy MMX6 capture files, overlay
interval containment rose from the direct-only **71.8%** baseline to **93.9%**
(636/677). Every one of the remaining 41 PCs is in the BIOS/kernel window;
including the separately recompiled, live-byte-guarded base BIOS raises combined
interval containment to **100.0%** (677/677) for that observed set. Validation
used only disposable `%TEMP%` caches.

Tomba 1's header-table exports require a different conservative retry. Those
authoritative addresses are commonly switch-case entries in the middle of a
function, so a direct-JAL-only retry loses the dispatcher while a broad scan can
walk into embedded data. The extractor pairs each export with its nearest
preceding prologue and bounds it to the proven producer. Call discovery proceeds
in stable rounds so an interior callee cannot split and truncate a later explicit
host. Dense in-image pointer tables are rejected even when their words happen to
decode as MIPS, while bounded return-to-return scanning recovers independently
valid unreferenced frameless leaves. A clean 25-record corpus produces 17 unique
shards with zero audit failures. Overlay ranges cover **820/823 (99.6%)**
historical-vault PCs and **410/450 (91.1%)** live-history PCs; every residual is
in the separately recompiled BIOS/kernel ranges, producing **100.0% combined
interval containment** on both observed scoreboards. The generated-C audit
remains the gate: rejected broad discovery is never installed.

Ape Escape's `KKIIDDZZ.HED` encodes contiguous
`size_sectors:12 | logical_sector:20` runs spanning sibling DAT then BNS files.
The same cross-member vote gate proves `0x80136000` (10 anchors), `0x8013D000`
(27), and two mirrored minigame EXEs. Ordinary PS-X EXE handling owns the latter;
the HED producer emits 44 BNS members. Together with two self-describing minigame
EXEs and the resident BIOS recipe, the disposable validation cache has all 47
candidates compiled with zero unsupported/bad targets.

Normal-mode PS-X discovery quarantines its one provenance-free fallback: the
image body start is excluded when the PS-X header declares a different entry.
This removes Ape MINI2's body-leading address/string table at `0x80100000` while
retaining its declared `0x80100C84` entry. All decoder-classified interior
entries remain ordinary candidates, and the extractor serializes their `F/R`
overlapping alias recipes instead of promoting them to hard call roots. A clean
Tomba 2 disposable rebuild compiled 53/53 candidates with zero unsupported/bad
targets and, together with the base BIOS, reached **100% interval containment**
against the finite full-playthrough vault and 765-entry append-only live history.
No prior MAIN manifest was present for the two regenerated shards. Position-fixed
framed scans likewise recover Psy-Q's exact
`lui R; load ...,off(R); addiu sp,sp,-N` entry after a previous return, replacing
the old root eight bytes late. This closes Tomba 1's historical `0x80110E30` gap
in the clean X00 variant without minting overlapping functions.

### Play-free isolated continuation recovery (2026-07-19)

Normal-mode discovery can prove a function-pointer or overlapping alias entry
whose body the stricter exact-entry partition cannot safely own. These entries
are not promoted into the shared region: doing so can hard-cap a sibling and
recreate the mid-function truncation/softlock class. After the shared DLL is
published, `compile_overlays.py` now schedules strong play-free gaps through the
existing isolated `dispatch_root` fragment path. Missing normal-mode
function-pointer entries are exact-entry demands; current-byte alias recipes are
attempted only when the entry lies outside every guarded interval in that
capture's runtime-valid, CRC-matching shard set. Coverage from another byte
variant, a mismatched DLL/manifest pair, or a missing DLL export cannot suppress
a current-variant attempt.

Strong missing roots that share the complete byte/recompiler recipe are compiled
as one supplemental multi-root shard rather than one DLL per root. Partitionable
generated-C/requested-entry/no-output/no-identity/cache-collision failures are
recursively bisected; after each successful half, the remaining roots are
filtered against the newly published coverage so the other half cannot duplicate
entries already reached. Compiler/toolchain failures stop that recipe once
instead of triggering an O(N) retry storm. Executed/forced roots and
static-interval recovery remain singleton failure domains. Every published batch
still requires every requested C definition, exactly one 1..16-range identity per
root, zero unsupported/bad targets, a successful host compile, and an atomic
DLL/manifest pair. A valid concurrent winner is preserved and then revalidated
for exact manifest, ABI, and pair identity. Deterministic audit rejection of an
unplayed static candidate is a memoized safe skip; the same rejection is fatal
once live execution or `--force-interior` makes it an exact demand.

### Cross-variant hosted interior recovery (2026-07-20)

Some overlay byte variants expose a static dispatch entry that another variant's
normal walk reaches only as an interior CFG block. Sibling evidence may nominate
that address, but it supplies no executable authority. The compiler first closes
all strong static roots across every merged byte recipe, then snapshots organic
donors globally before the hosted pass. That ordering makes a clean invocation
complete without relying on roots reloaded by a second run. Donors must be
CRC-valid `F` entries with exactly one positive range rooted at their entry and
explicit static control-flow provenance. Roots that also carry live, forced, or
interval evidence close as isolated singletons before the snapshot. Hosted and
post-snapshot orphan manifests carry pair-consistent non-authority provenance:
they remain normal runtime coverage, but neither their aliases nor incidental
rooted callees can become donor/owner authority on a later invocation. The
recipient must independently provide one unambiguous, rooted, single-range host
with current-byte CRC/geometry, the same producer, strict containment, and an
already existing organic block leader. Publication rechecks every requested host
and alias against the selected identity.

Nomination, host, batch, attempt, and emitted-identity limits are deterministic.
Per-recipient sibling streams exclude self and fixed authority coverage before a
cache-independent nomination window is capped. Supplemental coverage remains in
that fixed universe through owner selection and the alias/host caps, then the
compile-time `still_needed` guard removes it; repeated invocations therefore
cannot page through any later selection cap. Partial block-leader successes retry
their bounded remainder immediately. Cap drops remain safe interpreter fallback.
Hosted aliases are exact-entry dispatch candidates,
never CPS range owners: continuation lookup accepts only the candidate rooted at
the minimum guarded range address, and otherwise interprets.

The current `cg5_06162507` Tomba 1 proof starts from an absent cache and publishes
189/189 runtime-valid DLL/manifest pairs (`ok=189 failed=0`): 71 authority shards
plus 118 `hosted-v1` supplements and 11,177 candidate records. Repeating the exact
command publishes zero shards and leaves all 378 canonical DLL/manifest files
byte-identical with unchanged timestamps. Independent played-vault recall is
808/823 entry addresses (98.2%) and 793/867 exact `(entry, code_crc)` variants
(91.5%); the exact BIOS shard brings combined interval containment to 823/823. A
no-autocompile runtime smoke reached the village at 60 fps with active native
dispatch and no candidate/range/manifest overflow. CRC rejected two stale
candidates safely. Tomba 2, MMX6, and Ape Escape still require regeneration under
this hash; the results below are the prior common-hash checkpoint.

Tomba 2's clean `cg5_7125d9b5` build produced 53 region shards plus 104 accepted
continuation fragments (`ok=157 failed=0 skipped=5`). All 12 durable MAIN gaps
reproduced their historical `(entry, code_crc)` identities exactly, and the
resident shard added its six entries. Against the verified 888-entry append-only
history, combined overlay+base-BIOS interval containment rose from 870/888 to
888/888. This is finite-set interval containment, not exhaustive or exact-entry
coverage: exact native recall is 158/888 and remains the next resume-entry
target. A matching-hash static-only attract run passed the former sign and ledge
freezes, returned to title at approximately 60 fps, and recorded about 95.3%
native dispatch.

The bounded batching checkpoint was then rebuilt cleanly under the same final
`cg5_7125d9b5` hash. MMX6 produced 107/107 runtime-valid DLL/manifest pairs with
zero final failures and only 81 duplicate instances among 17,425 unique
`(entry, code_crc)` identities. Against 701 verified historical observations it
provides 588 exact entry addresses (83.9%), 595 with the exact BIOS resident
shard (84.9%), 94.2% overlay interval containment, and 100% combined interval
containment. Tomba 1 produced 74/74 runtime-valid pairs; its played-vault exact
entry-address recall is 808/823 (98.2%), while exact `(entry, code_crc)` recall is
778/867 (89.7%). Overlay interval containment is 820/823 (99.6%), and the exact
BIOS shard brings combined interval containment to 823/823. Interval containment
remains potential compiled-byte ownership, not proof of exact native dispatch at
an interior resume PC.

MMX6's full clean cache contained 17,506 raw manifest identities. Before
whole-pair deduplication, that physical-row total exceeded the runtime's old
16,384 process-lifetime candidate ceiling and motivated raising the table to
32,768, with all dependent hash/manifest/range tables scaled consistently. A
durable `candidate_overflow` status counter, loud loader message, and one-shot
per-bundle suppression make any future exhaustion explicit and fail closed to
the interpreter without repeated DLL loads. An adversarial lifecycle review and
the structural capacity regression test cover partial loads, zero-candidate
cleanup on both platforms, and speculative Windows image-map cancellation.
The deliberate desktop memory cost is +11.375 MiB of static BSS in the MinGW
`overlay_loader` object (37.297 to 48.672 MiB), mostly from the proportionally
scaled lazy indexes; non-desktop ports should account for this fixed allocation.

A fresh Ape validation rebuilt all **47/47** candidates with zero audit or host-
compiler failures. A full title-to-attract-to-title cycle exercised four overlay
generations and 107 dispatch PCs. The overlay cache covers all 72 exercised game
PCs by compiled range; the remaining 35 are BIOS/kernel PCs, and the separately
generated base BIOS covers them all (**100.0% combined interval containment** of
that observed set). Live-byte validation rejected 21 stale candidates without
executing them, while every loaded shard reported zero missing manifests and both loader
indexes stayed within capacity. Screenshots verified the rainbow/falling attract
scene and returned title screen rendered correctly. The dev persistence profile
produced 21/21 signed immutable snapshots with zero invalid or duplicate records;
production uses only the append-only executable-local history.

Shard publication is interruption-safe. GCC/tcc now writes a unique sibling
temporary DLL and the tool atomically replaces the content-keyed cache name only
after a successful, non-empty output. Cache reuse additionally requires a
non-empty `.ranges` manifest. A killed compiler therefore cannot leave a partial
final-named DLL that a later build mistakes for a valid shard, and a failed
`--force` rebuild cannot destroy the previous good DLL.

The `cg5_060368b2` discovery checkpoint was then regenerated from each disc and
cross-title checked in disposable caches. Ape GCC-built **47/47** candidates and
retained **107/107 combined live-history interval containment**. MMX6 GCC-built
**31/31** candidates and retained **677/677 combined interval containment**; broad
normal-mode attempts containing embedded data or an incomplete label graph were rejected and
the corresponding shards safely retried with direct seeds. Tomba 2's full set of
**53/53** candidates passed the generated-C audit with zero unsupported/bad
targets, and GCC built representative MAIN, large-area, OPN/CRD, and BIOS shards;
11 representative GCC shards were retained before the full-corpus pass switched
to the packaged compiler. That validation also exposed and fixed TCC-path parity
bugs: TCC now receives the same `PSX_NO_DEBUG_TOOLS` and block-cycle defines as
GCC, and a generated-source-only shim supplies the unsupported `__builtin_ctz`
without changing hashed runtime headers. TinyCC 0.9.27 then built the complete
T2 set **53/53**. Atomic publication also removes TCC's staged `.def` sidecars,
so successful and interrupted compiles leave only final DLL/manifest pairs.

## Next to raise coverage (future session)

1. ~~Direct-call frameless-leaf discovery~~ — DONE. Reachable `jal` targets are
   emitted as trusted `call_root` seeds; same-basic-block constant-register
   `jalr`/tail-`jr` targets are resolved with clobber checks. Exact-entry recall
   first rose by 74 MAIN.EXE entries. Exact-entry boundary verification now also
   retains normal-mode frameless leaves aligned by NOPs after a preceding return;
   this added 13 clean MAIN definitions and closed every remaining MAIN code-range
   gap. The generated-C audit remains zero unsupported / zero bad targets.
   Position-fixed producers also scan dense runs of at least three in-range
   function pointers. A target is promoted only when it independently has a
   prologue or preceding return boundary, so isolated pointer-shaped data and
   mid-function labels remain excluded. Read-only Tomba 1 regression data proves
   this finds its indirect-only leaves at 0x80111BB4/0x80111BCC; all 11 new
   Tomba 2 targets audit cleanly in their standalone image variants. Position-
   fixed extraction additionally verifies unreferenced candidates immediately
   after a prior return with the same bounded CFG proof; this recovers Tomba 1's
   final historical miss at `0x8010427C`. Stable-round exact discovery absorbs
   call-derived interior aliases into their existing host instead of using them
   as hard caps. Together these changes bring Tomba 1's combined historical and
   live observed-set scoreboards to **100.0% interval containment**.
2. ~~Header-table base recovery~~ — DONE (jal-fit, above).
3. ~~Recover weak-signal A09/A0J/GAME/OPN~~ — DONE. They are ordinary uncompressed
   MIPS, not archives. Counting only callable `jal` targets sharply resolves
   A09/A0J/OPN; GAME's 8-byte near-tie resolves through the exact base independently
   proved by DEMO. The played vault byte-proves GAME at 0x80106228. Ambiguous files
   without one unique trusted match still fail closed.
4. ~~AOT the game-independent kernel region once from the BIOS~~ —
   DONE without compiling the whole dirty kernel capture. The base BIOS recompiler
   supplies relocated guarded bodies and A0/B0/C0 stubs. The only invariant
   installed-code gap is the exact-hash `DF80-DFF0` resident recipe above. Padding
   and the adjacent `DFFC` callback pointer are intentionally excluded from code.
5. Generalize into a real `tools/` producer (no hardcoded paths; disc + game.toml in).
