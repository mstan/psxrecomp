# Host optimization contract

This document is the acceptance contract for changes intended to make
psxrecomp run faster on the host. It applies to generated code, the runtime,
the flat CPS trampoline, overlay dispatch and validation, graphics and audio
support code, and host-side caches or fast paths.

Correctness fixes and deliberate guest-visible enhancements still need focused
verification, but the 5% performance retention gate below applies specifically
to optimizations.

## Non-negotiable rules

1. Measure on a quiet host with zero compiler processes. Finish all AOT,
   overlay, shader, and helper compilation before the timed region. Confirm
   that no compiler, linker, build driver, or autocompile worker is alive while
   sampling. A run in which one appears is contaminated and is discarded, not
   explained away.
2. Interleave A and B. Never measure all baseline runs and then all candidate
   runs. The minimum acceptable order for three pairs is `A1 B1 A2 B2 A3 B3`;
   reversing the first member of alternate campaigns is encouraged. Keep the
   same binary and use a forced runtime selector when practical.
3. Report every raw run, the min-of-N for A and B, the percentage change
   between those minima, both medians, and the percentage change between the
   medians. N is at least three. State whether lower or higher is better.
4. Discard a pair when either member is contaminated. Reasons include a
   compiler process, overlay compilation, shader-cache construction, a changed
   power or thermal state, unexpected foreground load, a different overlay
   population, or a different pacing owner. Record the discarded pair and its
   reason in the experiment ledger.
5. A candidate that adds complexity, persistent state, generated-code ABI,
   configuration or debug ABI, or another maintained fast path is retained
   only when the agreed primary metric improves by at least 5%. A pure
   simplification may be retained at flat cost, but its measured numbers must
   still be recorded. Correctness fixes are judged on correctness, not this
   speed threshold; do not disguise an optimization as a correctness fix.
6. With the optimization forced on for every eligible operation, guest-visible
   state must be byte-identical to the faithful path at deterministic
   checkpoints. An automatic fallback is not proof: it can hide the exact case
   the fast path mishandles.
7. The faithful reference computation remains linked and selectable. A fast
   path is an implementation of that same computation, not a replacement model
   that merely looks plausible for one title.
8. Do not copy another console project's opcode handlers. It is valid to reuse
   its experimental process, evidence format, test structure, or review
   checklist. Implement R3000A behavior from PlayStation evidence and
   psxrecomp's own faithful path.

## Preparing comparable psxrecomp runs

Prefer one executable containing both paths and an explicit selector with
three states: faithful, candidate forced on, and normal policy. Record the
selector and assert in telemetry that the requested state took effect. If two
binaries are unavoidable, build both before the campaign and record their
commits, toolchains, options, and artifact hashes.

Static recompilation makes build quietness part of the benchmark definition.
Pre-generate the game and BIOS images, warm every overlay needed by the
workload, and prevent runtime autocompile during the measured interval. Record
the static image identity and the exact overlay inventory. A benchmark is not
comparable if A interprets an overlay while B uses a compiled shard.

Content-verified overlay variants require an additional check. A and B must
use the same captured bytes, identity ranges, flavor, cache generation, and
selected variant at each checkpoint. Pin and report the overlay flavor. A fast
identity check may avoid a CRC or range scan, but the faithful verifier must
remain callable and both must select the same content-verified variant.

The flat CPS trampoline is part of observable execution semantics. Compare the
published continuation PC, dispatch result, exception redirect, dispatch depth,
and same-site resume consumption at block and function boundaries. A host
function returning with `cpu->pc == 0` is not equivalent to tail-publishing the
next guest PC, even if common images never cross that boundary.

Use a deterministic workload with fixed inputs and a fixed capture or replay
when possible. Keep renderer, audio backend, window state, display refresh,
vsync/pacer ownership, CPU overclock, CRTC multiplier, mods, RAM size, and
instrumentation identical. Warm caches outside the timed region unless cold
behavior is the stated metric.

## Correctness proof

The forced-candidate run must exercise the candidate, and its checkpoints must
match a run forced through the faithful reference. Hash or serialize all state
that can affect later guest execution, including:

- GPR, HI/LO, PC, COP0, GTE registers and completion timestamps;
- main RAM, scratchpad, VRAM, SPU RAM, and relevant device registers;
- the R3000A cycle counter, deferred cycle charges, event deadlines, IRQ state,
  load-delay state, and pending exception continuation;
- dispatch and overlay identity state that changes which guest bytes execute;
- deterministic mod-owned guest state and savestate-visible sections.

Host-only counters may be excluded only when they cannot influence selection,
timing, invalidation, or later guest state. Document every exclusion.

Compare at more than the final frame. Include checkpoints before the candidate
first fires, immediately after it fires, at overlay or exception boundaries it
touches, and after a long soak. For caches and memos, force hits, misses,
invalidation, wrap or saturation, and savestate/rewind restore. For generated
code, cover the game emitter, BIOS full-function emitter, compiled overlays,
static overlay bakes, and the dirty interpreter wherever the contract crosses
execution tiers.

For cycle-affecting work, use `tools/cycle_testrom/measure.py` as the existing
measurement seam. Run the same test-ROM anchors against the native compiled
path, `PSX_FORCE_INTERP=1`, and the Beetle oracle as applicable. The script's
per-iteration deltas establish guest-cycle correctness; they do not replace a
separate interleaved wall-time or throughput campaign for host performance.
The faithful per-instruction R3000A cycle model remains authoritative: an
optimization may reduce host work but may not remove base charges, wait states,
interlocks, GTE completion timing, I-cache behavior, or peripheral deadlines.

## Performance campaign and evidence

Choose the primary metric before running the campaign. Frame or workload time
is preferred when the work is bounded; throughput is acceptable for a fixed
steady-state loop. Report secondary metrics, but do not change the retention
decision after seeing the results.

The evidence block for every candidate contains:

```text
Candidate: <short name>
Commits/artifacts: A=<hash> B=<hash> selector=<setting>
Workload: <capture, interval, overlay inventory, backend and host>
Primary metric: <name and unit; lower/higher is better>
Run order: A1 B1 A2 B2 A3 B3
Raw A: <a1>, <a2>, <a3>
Raw B: <b1>, <b2>, <b3>
Min-of-3: A=<min-a> B=<min-b> change=<percent>
Medians: A=<median-a> B=<median-b> change=<percent>
Correctness: faithful=<hashes> forced-on=<hashes> result=<identical/not identical>
Contamination: <none, or discarded pairs and reasons>
Complexity/ABI: <none, or exact added surface>
Decision: ACCEPT/REJECT - <reason>
```

Worked example, using workload time in milliseconds where lower is better:

```text
Run order: A1 B1 A2 B2 A3 B3
Raw A: 12.000, 11.000, 13.000 ms
Raw B: 9.744, 9.493, 10.100 ms
Min-of-3: A=11.000 B=9.493; improves 13.7%
Medians: A=12.000 B=9.744; improves 18.8%
Correctness: faithful=7a... forced-on=7a...; byte-identical
Complexity/ABI: two per-variant words plus one generated helper call
Decision: ACCEPT - both summaries exceed the 5% retention gate
```

Do not report only a profiler share, a best screenshot, an FPS range, or a
sequential before/after observation. Those are useful for finding candidates,
not for accepting them.

## Experiment ledger

Every investigated candidate gets a ledger row, including rejected,
superseded, reverted, and flat candidates. The ledger is append-only evidence,
not a highlights page. Keep it beside the optimization or campaign notes and
include links to raw data when available.

Required fields are:

| Field | Required contents |
|---|---|
| ID and date | Stable candidate ID and campaign date |
| Change | Commit(s), artifact hashes, selector, and concise hypothesis |
| Scope | Workload, renderer/audio backend, host, toolchain, config, overlay inventory and flavor |
| Correctness | Faithful and forced-on checkpoint hashes; exact exclusions |
| Runs | Interleaved order, all raw accepted runs, and every discarded pair with reason |
| Summary | A/B min-of-N and medians with percentage changes |
| Cost | Added code, state, ABI, configuration, maintenance burden, or simplification |
| Verdict | Accepted or rejected, threshold used, and reason |

Accepted and rejected results are equally visible. A compact ledger index may
look like this, with each row linking to its full evidence block:

| ID | Min-of-N change | Median change | State result | Cost | Verdict |
|---|---:|---:|---|---|---|
| EX-A | improves 13.7% | improves 18.8% | byte-identical | two per-variant words and one helper call | ACCEPT - clears 5% |
| EX-R | improves 3.0% | improves 1.0% | byte-identical | new cache and invalidation path | REJECT - below 5% |

Use an explicit rejected entry even when the candidate was reverted before a
formal campaign. Record the numbers that motivated the experiment and state
which required evidence is missing. Do not silently delete failed ideas: the
ledger prevents the same attractive but invalid optimization from being tried
again without new evidence.

## Review checklist

- Host was quiet and had zero compiler processes during every retained run.
- A/B was interleaved; contaminated pairs were discarded and recorded.
- Raw runs, min-of-N, and medians are present.
- The forced candidate and faithful reference produced byte-identical
  guest-visible state.
- The faithful path remains linked and selectable.
- Overlay identities and execution tiers were held constant and reported.
- Cycle-model changes were checked through the cycle-test-ROM seam.
- Added complexity or ABI clears 5%; flat pure simplifications have numbers.
- Accepted and rejected candidates are both in the experiment ledger.
- R3000A implementation was derived independently; no other console's opcode
  handlers were copied.
