# Overlay Recompilation — Design Review

**Status:** CONVERGED (post-review). The open questions below were taken to an
external reviewer; the resolutions are recorded in §5–§7 at the end, which
supersede any "OPEN" markers above. §0–§4 are preserved as the reasoning trail.

**Increment 3 (current work):** the design of record is **§8 — Per-entry
validity + multi-candidate dispatch**, which supersedes the coarse region-level
guard of §5-A *for Inc3* and demotes segmentation (§2.6) from a correctness
mechanism to a coverage one. Read §8 first if you are working on the
native-speedup / reload problem.

---

## 0. Context (for a reader unfamiliar with the project)

PSXRecomp is a **static recompiler** for PlayStation 1 games: it translates the
game's MIPS machine code to native C ahead of time, compiles that to a real
binary, and runs it natively (no CPU interpreter on the hot path). This is the
N64Recomp / SuperMarioWorldRecomp model. The project premise — non-negotiable —
is that **the game runs as statically recompiled native code**, not emulation.

The hard part: PS1 games stream **overlays** — code segments DMA'd from disc
into RAM at runtime, executed, then overwritten by the next overlay. A pure
static recompiler can't see overlay code at build time (it isn't in the main
executable; it arrives at runtime). Two existing mechanisms:

1. **Dirty-RAM interpreter** (`dirty_ram_interp.c`): a small MIPS interpreter
   that runs any code in RAM pages written at runtime. This is the *failover* —
   correct but slow, and explicitly **not** the goal.
2. **Overlay DLL cache** (the subject of this doc): capture overlays as they
   load, recompile them offline to native DLLs keyed by content hash, and on
   later runs `LoadLibrary` the DLL and dispatch its functions **before** the
   interpreter. Coverage grows as people play.

### The pipeline as built
```
runtime capture  ──► overlay_captures.json (full bytes + executed PCs + dispatch PCs)
       │                         │
       │                  compile_overlays.py (offline)
       │                         │  - classify seeds (which PCs are real function entries)
       │                         │  - run the recompiler on the overlay bytes
       │                         │  - audit generated C, compile to <crc>.dll
       ▼                         ▼
  dirty-RAM page          cache/<game_id>/<region>_<crc>.dll
       │                         │
       └──► dispatch: overlay DLL func (if registered) ──► else interpreter (failover)
```

---

## 1. The governing principle we've converged on

**Asymmetric failure ⇒ bias for precision over recall.**

- A **missed** overlay function is *safe*: the loader only claims the specific
  `func_XXXXXXXX` addresses it registered from the DLL. An unregistered address
  returns "not handled" and **falls through to the interpreter**. The seed is
  then captured and the overlay is regenerated next time with that seed
  included. Under-coverage self-heals.
- A **mis-compiled** function is *fatal*: it gets registered, the game calls it,
  and it executes wrong native code against shared CPU state → corruption →
  black/blue screen. There is no failover once we've claimed an address.

Therefore every design choice should **minimize false registrations**, even at
the cost of compiling fewer functions per pass. Recall is recovered for free by
the interpreter-failover-and-recapture loop; precision is not recoverable.

**Question for review:** Is this asymmetry actually total? Are there cases where
a *missed* function does NOT safely fall back — e.g., the game's control flow
assumes the function executes within some timing budget that the interpreter
blows, or the missed function is on a path where the interpreter itself can't
handle some instruction? If "miss is always safe" has exceptions, the whole
precision-first thesis weakens.

---

## 2. Converged positions (please attack)

### 2.1 Capture is complete and data-inclusive; the DLL is code-only
- The on-disk capture artifact contains the **full overlay image, including
  data** (jump tables, constant pools). Required because (a) re-recompiling
  later with better heuristics needs the original bytes, (b) the recompiler must
  *read* data to resolve jump tables, (c) the cache key is a CRC over the image.
- The compiled **DLL contains code only**. The overlay's data is never emitted;
  it lives in PSX RAM (the game DMA'd it there), and recompiled functions read
  it through normal memory callbacks at runtime.
- Consequence: "how much benign data surrounds the code" is **not** a
  correctness question for the DLL. The only correctness question is "is this
  word reached as code?" Data is recognized as not-code and left alone.

### 2.2 Capture bytes: DMA-time vs execution-time (OPEN — leaning a direction)
The current capture stores **DMA-time ("unpatched")** bytes — the overlay as
delivered from disc, *before* any load-time relocation/fixups the game applies.
If a game patches the overlay after load (relocation, jump-table base fixups,
self-modification), the bytes we recompile ≠ the bytes that actually executed,
and the DLL is subtly wrong. This is a candidate root cause for past black/blue
failures that has nothing to do with seed classification.

- **What's correct to recompile from:** execution-time bytes (what the CPU
  actually ran).
- **Why not just snapshot RAM at exit:** by exit, that RAM range may already be
  contaminated by a *later* overlay reusing the address.
- **The right moment is first-execution-time:** snapshot `RAM[region]` the first
  time we dispatch into it — after fixups, before reuse.
- **Proposed:** in dev builds, dump **both** (`<crc>_dma` and `<crc>_exec`) and
  diff them. Identical ⇒ no runtime patching, capture-time is irrelevant.
  Different ⇒ the diff reveals the fixup pattern and likely the bug.

**Question for review:** Is "recompile from execution-time bytes" correct, or
does it break the content-hash cache model (the patched bytes may be
load-address-specific — see §2.5 relocation)? Should the cache key be over
unpatched or patched bytes?

### 2.3 Function discovery is one evidence hierarchy; no whole-byte sweep
Rank evidence that an address is a real function entry:
1. Known entries (initial PC, exported symbols, declared entries)
2. **Executed PCs** — we literally watched them run (strongest signal)
3. Direct `JAL` targets (statically provable callers)
4. Jump-table targets we resolved by reading the table
5. ~~Whole-byte sweep: decode every byte hoping to find functions~~

Tier 5 is the *only* unsound tier for overlays — it invents entries from data,
which is what produces the data-as-code failures. We have tiers 1–4 for free on
overlays (we captured them by running them). So discovery should use 1–4 and
**never** tier 5 for overlays. (The cold main EXE, with no execution evidence,
is the only place tier 5 has ever been justified — and even there it should be
the flagged, lowest-confidence tier.)

This was previously implemented as an opt-in recompiler flag
(`--overlay-exact`). We believe a flag is the wrong abstraction: evidence-driven
discovery is *implied by whether execution evidence exists for the input*, not a
human toggle.

### 2.4 Mid-function seeds: capture them, merge at encode time
A runtime "dispatch entry" can be a **jump-table case label mid-function**, not
a callable function boundary. These are dangerous: the game *does* reach them
(so we'd register them), but compiling them as standalone functions yields wrong
boundaries/stack assumptions.

- Don't filter at capture time (the runtime can't tell). **Capture everything.**
- At encode time, with the full bytes, classify each seed: a seed that falls
  strictly inside another function's `[start,end)` is **mid-function → merge
  into the parent** (it becomes a label in the parent's CFG, not its own func).
- The discriminators are local and don't need external callers: preceded by
  `jr $ra`+delay slot (real boundary), has a prologue, is a direct `JAL` target.

### 2.5 Confidence flagging ("jump-risk")
The ambiguous residue: a function reachable **only** via a computed/indirect
jump (jump table or function pointer), never via a direct `JAL`. It looks
identical to a jump-table case label. Proposal: tag each emitted entry with a
confidence (`strong` / `mid-function` / `jump-risk` / `forced`) in a
machine-readable sidecar (the recompiler already emits `skipped_functions.json`,
so the pattern exists). 

**Open sub-question:** for `jump-risk` entries, do we (a) emit them with the
flag, or (b) *exclude* them from compilation initially and let them
interp-fallback until a direct-JAL or execution proof confirms them? Given the
precision-first thesis, (b) is tempting.

### 2.6 Overlay segmentation and cache keying (OPEN)
The runtime currently captures **one region = all contiguous dirty pages**
(observed: a single 356 KB region for Tomba). That is almost certainly *not one
overlay* — it likely merges several disc loads plus unrelated dirtied RAM.
Problems: the cache is CRC-keyed per region, so if a later run loads a different
overlay into part of that range, the CRC changes and the whole region misses.
Proposal: re-segment using the CD-DMA transfer log (each forward CH3 DMA records
LBA→dest→size) into real per-load overlays, and key the cache per overlay.

**Question for review:** can disc-load boundaries (CD-DMA log) reliably bound
overlays, given overlapping/reused address ranges and overlays assembled from
multiple DMA blocks? Is per-(load_addr, crc) keying sufficient, or does
relocation (§2.2) force something else?

**RESOLVED (see §8).** The framing above conflated two separate jobs. Keying the
*cache lookup* and bounding *what's safe to run natively* are different axes:
- **Validity / reload-on-return is now per-function** (§8), tracked by a content
  hash of each function's own bytes — so it no longer matters whether the region
  is cut perfectly. A merged 1.1 MB blob is correct; each function self-validates.
- **Segmentation is demoted to a *coverage* concern**: its only remaining job is
  ensuring every *distinct* overlay that reuses an address (Tomba reloads village
  and overworld both into 0x800E7000) actually gets compiled, rather than the
  cumulative dirty-bitmap folding them into one image. The boundary signal is
  **mechanism-agnostic and uses no per-game knowledge**: a region that was
  *written then executed*, when later *rewritten*, seals as one overlay
  generation (snapshot bytes + exec seeds, content-hash it) and the next
  generation accumulates fresh. The "rewrite" event is the same store-path
  invalidation signal we already own — never a hooked game/BIOS install routine.
  The earlier note about pc=0xBFC12210 was diagnostic only; nothing keys on it.

### 2.7 Code hygiene (DRY)
The same low-level MIPS primitives (`jr $ra` detect, `addiu $sp` prologue,
valid-MIPS-word, classify-control-flow) are currently reimplemented in at least
three places (the Python seed vetter, the Python overlay classifier, the C
runtime decodability guard). The *policies* differ legitimately (single-address
verdict vs. whole-overlay CFG classification) and should stay separate, but the
primitives should be one shared module.

---

## 3. The specific failure we just observed (concrete grounding)
- Captured one overlay: load `0x800E7000`, 356 KB, 15358 executed PCs, 66
  dispatch-entry seeds, full bytes present.
- The offline classifier selected 122 function entries + 59 direct-JAL targets
  and excluded 3096 branch targets + 14221 observed-only PCs (mid-function
  filtering already works).
- But the recompiler, run **without** evidence-scoping (whole-byte sweep), walked
  into embedded data tables and emitted `UNKNOWN_BAD` jump targets +
  `UNSUPPORTED_INSTRUCTION` runs. The offline audit then refused to emit the DLL
  (all-or-nothing per region) → **zero DLLs produced**.
- So the observed failure is at the **discovery strategy** (tier-5 sweep
  decoding data), not at seed classification. This is the direct evidence behind
  §2.3.

---

## 4. The specific questions we most want challenged
1. Is the precision-over-recall asymmetry (§1) truly total, or are there
   "missed function does not safely interp" cases?
2. Execution-time vs DMA-time capture (§2.2): which should we recompile from,
   and which should key the cache, especially under relocation?
3. Does relocation/self-modification break the static-overlay premise for some
   overlays — i.e., is there a class that *must* stay interpreted forever, and
   how do we detect it cheaply?
4. Jump-risk entries (§2.5): emit-with-flag vs exclude-until-proven?
5. Segmentation (§2.6): can the CD-DMA log reliably carve real overlay
   boundaries out of the dirty-page blob?

---

## 5. Resolved — converged decisions (post-review)

Accepted refinements (now design-of-record, not open):

- **(A) Runtime guard = write-invalidation, not CRC-per-dispatch.** On DLL
  load: validate code + dependency ranges once. On any write to a *watched*
  RAM page: invalidate the affected registered entries / DLL range. On
  dispatch: registration-table check only, no hot-path CRC. Coarse page-level
  invalidation first; false positives (over-invalidation → interp fallback)
  are acceptable. Do NOT start with fine interval invalidation.
- **(D) Internal labels are compiled inside a proven parent, never
  registered.** A jump-table / branch target is *not* a callable function
  entry; it is compiled as a label inside a call-proven parent function and
  reached via the parent's native switch. This is what keeps precision-first
  from degenerating into "interpret all switch code."
- **(B) Edge capture, minimal schema** — at the control-flow classifier, not
  the dispatcher. `{src_pc, dst_pc, edge_type ∈ {Jal,Jalr,Jump,Branch,JrRa,
  JrOther}, writes_ra}`. Defer `sp_before/after`, `delay_slot_pc` until a
  concrete classifier failure demands them. Share the edge enum with the
  recompiler's static walk.
- **(C) `UNSUPPORTED_INSTRUCTION` is a boundary/CFG suspicion first, real
  opcode gap last.** Classify in order: (1) reachable from entry via valid CF?
  (2) fell through past `jr $ra`/terminal branch into data? (3) in a reachable
  branch/jump delay slot? (4) impossible target from bad discovery? (5) only
  then a true opcode gap. Trim only when justified by an actual terminal
  control-flow edge — never "trim until the audit passes."
- **(E) Native-vs-interp differential testing** is the empirical backbone for
  the parity assumption — but it is increment-2, not an increment-1 blocker.

### The three-bucket entry rule
```
direct JAL target / jalr-with-link / declared / initial entry
        -> compile as function, REGISTER as callable
jump-table or branch target inside a known parent CFG
        -> compile as label inside the parent, DO NOT register
observed dispatch target with no call-edge / parent proof
        -> DO NOT register; interp fallback; capture better edge evidence
```

### The load-bearing invariant
```
Compiled  != Registered.
Emitted   != Callable.
Only REGISTERED entries may bypass the interpreter.
```
The DLL may physically contain functions for diagnostics/future use; what the
runtime registration table claims is the only thing that affects correctness.

### Precision-over-recall — refined (answer to §4 Q1)
The asymmetry is not a law; "a miss is safe" holds only if native↔interp state
preservation, instruction coverage, delay/load-slot and IRQ/DMA-timing
equivalence, and stale-registration invalidation all hold. But the thesis
survives in its correct form: **false registration is immediately corrupting;
missed registration is recoverable if interpreter parity holds; therefore
optimize for registration precision.** Parity itself is to be *measured* (E),
not assumed.

## 6. RAM-write-path audit result (gates the §5-A guard)

- All CPU stores (generated + interpreter) and DMA writes funnel through
  `psx_write_{word,half,byte}` in `runtime/src/memory.c`. The only `memcpy`/
  `memset` into memory target device-local buffers (`param_fifo`, `vram`,
  `spu_ram`), never main `ram`. **No native path mutates main RAM behind the
  store functions** ⇒ write-invalidation has no hidden false-negative source.
- Existing dirty tracking on the store path is **kernel-only**
  (`dirty_ram_mark_kernel_write` bails at `phys >= 0x10000`). Overlay pages are
  marked only by DMA (`dirty_ram_mark_executable_range`). So overlay CPU
  self-modification is currently untracked; the bitmap covers all 2 MB, the
  guard simply doesn't set it there.
- **Required change for §5-A:** a watched-overlay-pages bitmap set at
  registration; one extra bitmap test in the RAM-store branch of all three
  `psx_write_*`; on hit → `overlay_loader_invalidate_page()`. Coarse, cheap.

## 7. Implementation increments

**Increment 1 — make DLLs safe and loadable (current)**
1. No whole-byte sweep for overlay inputs (mandatory, ingester-set `--overlay`
   contract; not a human flag).
2. Split classification: code-reachable / callable entry / internal label.
3. Per-function (or SCC) audit, not per-region.
4. Register only proven callable entries.
5. Jump-table/case labels compiled only inside their proven parent.
6. Coarse write-invalidation for registered overlay pages (per §6).
   Success: Tomba emits a partial DLL; runtime loads it; only strong entries
   registered; unproven code interp-falls-back; no black/blue from false
   registration.

**Increment 2 — make it measurable**
Edge-capture schema (B); DLL manifest with code/dependency ranges;
registration-time validation; native-vs-interp differential harness (E).

**Increment 3 — make it efficient and stable**
Core: **per-entry validity + multi-candidate dispatch (§8)** — `PC → candidate
list`; the unit of "safe to run natively" moves from the whole region to the
individual compiled entry, decided by a lazy dependency-set hash gated on page
generation counters. Multi-candidate dispatch fixes reload across overlays that
reuse the same address. This delivers native speedup, kills false-invalidation
churn, and makes reload-on-return gradual and automatic. Supporting: jump-table
behavior check gates code-only vs manifest hashing (§8.3); active-entry
self-modification blacklist (§8.5); segmentation demoted to *coverage* (§8.6);
execution-time vs DMA-time dual capture; CD-DMA as provenance only.

---

## 8. Increment 3 — Per-entry validity + multi-candidate dispatch (design of record)

This section supersedes the coarse region-level guard of §5-A **for Increment
3**. §5-A's coarse per-region invalidation was the correct *Increment 1* choice
(it shipped the OV-1 fix: no more stale-native blue screen). Inc3 refines the
granularity now that correctness is established.

The previous segmentation-first plan was directionally right but attacked the
symptom at too high an altitude. The immediate blocker is not merely that the
capture region is large; it is that **validity is tracked at region granularity
while dispatch is per-function**. A compiled native entry corresponds to a
specific set of bytes the recompiler consumed, so validity should be tracked per
compiled entry, not per dirty region.

### 8.0 Why region-level validity failed (reasoning trail)

The Inc1 guard tracks one `valid` flag per `OvlRegion` over a single
`[fn_lo, fn_hi)` envelope. Two consequences make native speedup impossible:

1. **False-invalidation churn.** The envelope is a 343 KB span with the
   overlay's *data interleaved between functions*. The overlay writes its own
   state into that data during normal play; those writes land inside the
   envelope and nuke the *entire* region's registration, even though no code
   byte changed. Native execution collapses to interp almost immediately.
2. **All-or-nothing reload.** Reload-on-return requires the whole 343 KB span to
   hash-match atomically before *anything* goes native again. The rewrite
   threshold (`span/2 ≈ 171 KB`) never trips, and a hash that includes mutable
   data would never match anyway. `revalidations` is provably 0.

The root error is the *granularity*, not the region size. Fixed-size
alternatives (N-KB chunks, modulo-512 KB) were also rejected: they impose an
*artificial* grid (a function spanning a chunk boundary makes ownership
ambiguous; per-chunk reload implies per-chunk compilation → cache litter). The
function boundary is the natural unit and is already known to the recompiler.

### 8.1 Core model — PC → candidate list

Replace the current single-entry dynamic table:

```text
PC -> native function pointer
```

with a candidate list:

```text
PC -> [candidate native entries]
```

This is required because different overlays can reuse the same RAM address —
village and overworld content may both define code at `0x800E7xxx`. A
single-entry table lets whichever DLL registered last clobber the earlier
candidate, making reload-on-return impossible even if the earlier bytes reappear
in RAM.

Each candidate carries:

```text
entry_pc
native function pointer
dependency ranges
dependency hash
last validated page generation(s)
valid / invalid / blacklisted state
source DLL identity
```

Dispatch becomes:

```text
candidates = entries_by_pc[pc]

for candidate in candidates:
    if candidate is blacklisted:
        continue
    if candidate is still valid for current page generations:
        call native candidate
    else if candidate dependency hash matches live RAM:
        mark candidate valid for current generations
        call native candidate

fall back to dirty-RAM interpreter
```

A hash match proves only that this previously-approved native candidate is
applicable to the current RAM contents. It does **not** prove that an arbitrary
PC is a callable function entry — the existing precision rules (three-bucket
entry rule, §5) still govern what may be registered as a candidate. **Hash
validity must not imply function-entry validity.**

### 8.2 Validity unit — the dependency set

Validity is per entry, but the hash must cover exactly what the recompiler
consumed at compile time:

```text
reachable code bytes/basic blocks for this entry
plus compile-time-resolved analysis data, if any
```

Examples of compile-time-resolved analysis data:

```text
jump tables baked into generated native control flow
function-pointer tables resolved during recompilation
constant tables used to shape the generated CFG
```

Do **not** include ordinary overlay state/data that the generated native code
reads through runtime memory callbacks. Hashing live-read data would reintroduce
the false-invalidation churn we are moving away from and defeat the purpose.

```text
compile-time-consumed data -> dependency hash
runtime-read data           -> not part of validity hash
```

### 8.3 First gated work item — verify jump-table behavior

Before implementing the heavier manifest path, verify how overlay codegen
currently handles jump tables / indirect branches:

```text
Does overlay codegen bake jump-table contents into generated native control flow,
or does it dispatch indirect targets at runtime through psx_dispatch_call / equivalent?
```

- If jump tables are **not** baked and indirect control flow goes through runtime
  dispatch, **code-only dependency hashing is sufficient** for the first
  implementation (and the manifest can be deferred).
- If jump tables / other data-driven CFG facts **are** baked, a minimal
  Inc2-style manifest must be pulled forward so the recompiler emits per-entry
  dependency ranges.

**The loader must not guess dependency ranges. They must come from the
recompiler.**

**RESOLVED — code-only hashing is sufficient.** Both emitters (overlays use
`CodeGenerator`, `recompiler/src/code_generator.cpp`; `main_psx.cpp:378`) emit
indirect jumps as `switch (cpu->gpr[jr_rs]) { case <resolved>: goto block; …
default: call_by_address(cpu, jr_rs); }` — keyed on the **live runtime register
value**, not on a table index, with a runtime-dispatch `default`. The register is
loaded from the table at runtime by the normally-emitted `LW`, so a table rewrite
is reflected in the switched value: a match goes to the (genuinely correct)
target, a miss falls to runtime dispatch. Jump-table contents are therefore a
compile-time *optimization*, never a correctness-bearing dependency. `jalr`
(`code_generator.cpp:1235`) is pure `call_by_address` runtime dispatch. **The
dependency set for an overlay entry is its reachable code bytes only.** The
manifest can stay deferred to Inc2.

### 8.4 Lazy verification — generation counters

Writes stay cheap. On write:

```text
bump page_generation[page]
```

Do not hash on the store path. On dispatch:

```text
if candidate's dependency pages have unchanged generations:
    call native
else:
    hash candidate dependency ranges against live RAM
    if hash matches:
        update candidate's validated generations
        call native
    else:
        mark candidate invalid
        try next candidate or fall back to interpreter
```

Page generation counters are preferred over boolean suspect bits: they avoid
stale-clearing/race bookkeeping and make each candidate's validation epoch
explicit.

### 8.5 Active self-modifying entry handling

Lazy verification is sound across dispatch boundaries but cannot recover from a
native function that modifies its own code/dependency bytes and then continues
executing modified code *within the same activation*. Track the currently
executing native overlay entry. On any write:

```text
if write overlaps current native entry's dependency ranges:
    blacklist that entry from native dispatch
    increment selfmod_active_entry counter
```

This is **not** a recovery mechanism for the current activation — it is a safety
classification: that entry joins the "must remain interpreted" class until a
deopt/restart mechanism exists.

### 8.6 Segmentation is demoted, not deleted

Per-entry validity removes segmentation from the immediate *correctness* path: a
merged region no longer has to atomically remain valid to keep individual
functions native. But segmentation is still **load-bearing for coverage and
cache hygiene**:

```text
discovering distinct overlays that reuse the same address
creating multiple candidates per PC
keeping cache artifacts small
avoiding repeated compilation of giant merged captures
improving dependency manifests
```

The boundary signal stays **mechanism-agnostic, zero per-game knowledge**: an
overlay is RAM that was *written, then executed*; its lifetime ends when that RAM
is *rewritten* (the same store-path invalidation signal we already own,
`overlay_loader_invalidate_at` — never a hooked install routine; the earlier
`pc=0xBFC12210` note was diagnostic only).

### 8.7 Priority order

```text
1. Implement PC -> candidate-list dispatch.
2. Implement per-entry validity using code-only hash if codegen permits it.
3. If codegen bakes jump tables/data, pull forward a minimal manifest for
   compile-time dependencies.
4. Add active-entry self-modification blacklist.
5. Then return to segmentation for coverage / cache hygiene.
```

Corrected framing:

```text
Region-level validity caused false invalidation and blocked reload-on-return.
Per-entry validity fixes the runtime applicability problem.
Multi-candidate dispatch fixes address-reuse reload.
Segmentation remains necessary for good candidate coverage, but is no longer
the primary correctness mechanism.
```

### 8.8 Validation (no symptom patches)

village→overworld→village must show `disp_native` sustained across the round
trip (gradual per-entry reload), `invalidations` no longer spiking from in-region
data writes, and both overlays producing distinct compiled units / candidates.
Reload is proven by counters **and** pixels, never by a forced revalidation or a
hand-tuned threshold.

## 9. The dispatch call contract (Bug A case study — RESOLVED 2026-06-09)

### 9.1 The invariant

Execution moves between three engines: statically-recompiled C (main EXE /
BIOS), native overlay DLL code, and the dirty-RAM interpreter. A guest CALL
(jal/jalr) carries an obligation: when the callee completes, execution resumes
at the call's return address. That obligation is representable two ways:

1. **C-stack continuation** — the caller is itself a native C function; the
   generated call site is `call_by_address(target); goto continuation;`. The
   obligation lives on the host C stack. The callee's plain `return` (pc==0)
   correctly resumes the caller.
2. **pc-chain continuation** — the caller is being interpreted; the
   obligation lives in guest state (the ra register / the interpreter's
   `*next_pc_out = return_pc`). The callee must hand back a guest pc.

**The invariant: every path that executes a CALLEE as a unit (a native C
function that returns with pc==0) must itself hold the continuation — either
on the C stack or via explicit `resume at return_pc` logic. Surfacing a CALL
to the psx_dispatch_impl tail-call loop as a bare `cpu->pc = target` violates
this: the loop treats the callee's pc==0 return as "unwind to the C caller,"
and the suspended interpreted caller's continuation is silently dropped.**

### 9.2 Bug A (dwarf village → overworld native blue screen)

- Main-EXE field dispatcher F=0x800338A8 (prologue `addiu sp,-0x18`, jr-v0
  switch over `jumptable[0x80010748 + state*4]`, per-case `jal <overlay
  handler>`, joint epilogue at 0x80033954) is reached only via a data-table
  jalr; static discovery walked it from 0x338B0 (two insns late), so dispatches
  of 0x338A8 miss the static table and the dirty-RAM interpreter executes F.
- F's interp hit `jal 0x8011B0D4`. The jal handler preserved the call contract
  for statically-compiled callees (psx_dispatch_game_compiled path) and
  non-local targets (dispatch_nonlocal_call passes return_pc as stop), but a
  LOCAL DIRTY target fell through to `cpu->pc = target; return 1` — a bare
  pc-chain. The dispatch loop then ran the target as a NATIVE overlay
  candidate; its C-style return (pc==0) unwound the loop past F's suspended
  continuation. F's epilogue never ran: 0x18 of guest stack leaked per entity,
  deterministically, until the world corrupted (blue screen). Interp-only mode
  is immune because the callee's own `jr ra` is interpreted, keeping the
  pc-chain intact.
- Refuted by measurement before the fix: IRQ-check cadence (suppress armed,
  902 checks suppressed, identical failure), device-event granularity
  (conservative stepping, identical failure), and the jr-jumptable default
  fallback (the failing execution took a short path that never reached a
  jump table).

### 9.3 The fix

`overlay_loader_call_native()` (overlay_loader.c) + calls in the interpreter's
jal AND jalr handlers (dirty_ram_interp.c), mirroring the existing
psx_dispatch_game_compiled contract exactly: try the native candidate, and on
completion `*next_pc_out = return_pc`. Fingerprint logging is preserved at the
new call site. Side effect: native coverage jumped from 86 native overlay
calls per village→overworld run to ~112,000 (interpreted jal/jalr now route to
native candidates), since the old path only went native when the dispatch loop
happened to land exactly on a candidate entry.

### 9.4 Verification + measured benefit (2026-06-09)

- dwarf→overworld renders and plays under native overlays (user-verified
  pixels, Rule 5). 14 candidates valid, 0 invalidations, no wedge.
- Same-scene A/B on the live overworld: 646 vs 645 frames / 10 s (vsync-locked
  — fps cannot show the win); host CPU cost 7.31 (native) vs 7.83 (interp)
  CPU-seconds per 15 s wall ≈ 7% of total process CPU on this scene. The win
  is real but bounded by coverage: one cached DLL, 195 candidates, and ~93K
  interp fallbacks remain (main-EXE dirty-text code + non-candidate overlay
  functions).

### 9.5 Known residue

- Discovery-miss class: main-EXE functions reachable only via data jump
  tables (e.g. 0x80010748 — static data in the EXE) are never discovered and
  interp forever. A discovery pass over static jump-table data closes both a
  perf gap and this bug-class exposure.
- One unexplained one-off: a single black screen at the load screen on the
  first post-fix run (process exited via atexit; its watchdog dump shows only
  the known frame-~184 instant-disc boot transient). Not reproduced.
- §8.3's "table contents are an optimization, not a correctness dependency"
  now rests on the call contract above; the default fallback is a
  tail-dispatch and is correct ONLY because tail jumps carry no return
  obligation. Any future emission of the default in non-tail position would
  reintroduce this bug class.
