# CFG metadata repair: validation and scope

2026-09-12; PR [#354](https://github.com/RetroPortingToolKit/psxrecomp/pull/354).
Tracking: `beads-eio.3.148`. Original baseline: `3a275d49397ba2e2f4cc4399d76ffd47465e8d44`.

## Confirmed defects and repair

The live analyzer omitted reverse edges and called any backward-address edge a
loop. The real MIPS fixture `80010000 -> 80010020 -> 80010010 -> return` reproduces
both errors without a game asset. The separate BasicBlockAnalyzer was not the
affected analyzer. No retail-game failure is claimed to have been caused by this.

Rebuild unique in-graph reverse edges, declared-entry reachability and natural
back edges proven by dominance. Include independent alias entries through a
synthetic root. Preserve successor ordering, instruction ranges and ownership.
An iterative traversal avoids recursive stack growth; storage is O(V+E).
Final codegen fallthrough guards now consume reachability, since unreachable
blocks may also have predecessors. Block-level CPS/indirect handoffs are unchanged.

This graph is incomplete for runtime indirect/CPS entry. Neither one predecessor
nor a natural-loop label authorizes eliminating cache, slice, IRQ or device checks.
See [LLVM's loop definition](https://llvm.org/docs/LoopTerminology.html#loop-definition).

## Automated checks

- All 67 enabled recompiler CTests pass on the combined source tree, repeated
  after rebasing onto the offline build fix. Three pre-existing disabled tests
  (`interpreter_perf_guards`, `runtime_perf_diag_guards`,
  `overlay_pair_dedup_runtime`) are not counted as passes.
- Real MIPS fixtures, alias roots, duplicate/external edges, true forward-address
  back edges, nested/multiple-latch loops, irreducible/unreachable cycles,
  idempotence and a 12,000-block chain pass.
- 1000 deterministic random graphs agree with an independent vertex-removal
  dominance oracle, not a second copy of the production algorithm.
- Generated-tail cases distinguish unreachable incoming edges from reachable
  entry/alias paths. Existing emitter, overlay ownership and BIOS tests pass.

Configure the recompiler with `BUILD_TESTING=ON`; build, then run
`ctest --test-dir <build> --output-on-failure`. Stage the normally generated
runtime codegen-hash header before tests which compile a runtime overlay fixture.

## Fresh title builds and bounded live regression

Clang 22.1.8, Windows x64, RelWithDebInfo, OpenGL/headless, debug tools ON,
netplay/launcher UI OFF, no game mods, normal guest timing defaults. Fresh game
and SCPH-1001 BIOS generation; retail LLE BIOS boot (not BIOS HLE). The existing
deterministic HLE thread scheduler remains unchanged; this is not a claim of
an entirely HLE-free runtime. Each variant has isolated saves/build/cache data.
The warm run reuses that variant's cold-run overlay cache.

Master could not compile this offline configuration because it unconditionally
required netplay headers and account symbols. PR [#355](https://github.com/RetroPortingToolKit/psxrecomp/pull/355)
was applied identically to baseline and fixed runtime sources. Thus the A/B
comparison isolates CFG changes, not different build prerequisites.

| Title | Dispatch entries (both) | Native builds | Baseline cold/warm frames | Fixed cold/warm frames |
| --- | ---: | --- | --- | --- |
| Tomba! USA SCUS-94236 | 2989 | both pass | 11003 / 11003 | 11000 / 11004 |
| Mega Man X6 USA v1.1 SLUS-01395 | 2494 | both pass | 11015 / 11000 | 11026 / 11016 |
| Ape Escape USA SCUS-94423 | 3094 | both pass | 11005 / 11005 | 11004 / 11011 |

All twelve runs reached the 11,000-frame threshold, exited 0, reported native
overlay dispatch, nonzero guest SPU samples and zero kernel-bless mismatches.
Inspected screenshots show Tomba FMV/title and MMX6/Ape gameplay attract demos.
Polling captures nearby frames, not identical deterministic snapshots; no
frame-perfect or guest-state equivalence claim follows from those pictures.
Headless host audio output is inactive: guest SPU activity is not a listening test.

Dispatch tables, declarations and guest code-range manifests are byte-identical
between variants for each title. Codegen identity changes from `30317be3` to
`4fe894d2`, so their generated overlay caches are separated. Added final safety-net
code changes generated tails; one Tomba unknown-function stub moves between
adjacent shards because of the changed shard line budget, without losing its entry.

Evidence remains local under `_validation-fmv-prs/{baseline,fixed}/{tomba,mmx6,ape}`:
isolated `game.toml`, generated output, native builds and `results/retail-{cold,warm}`
JSON reports/screenshots. No copyrighted asset, capture, profile or save is committed.

## Merge boundary

This is a class-level metadata/codegen correctness repair, not an FMV speedup.
Local suite coverage supports integration but does not prove absence of regressions
in unvisited levels, other platforms, online play or full playthroughs. No GitHub
checks were reported. Tomba2 and all game-repository pins are deliberately untouched.

Keep [#356](https://github.com/RetroPortingToolKit/psxrecomp/pull/356) (opt-in IPO/PGO validation)
and [#357](https://github.com/RetroPortingToolKit/psxrecomp/pull/357) (IRQ batching RFC and cache
counterexample) in draft. No proposed IRQ/slice/I-cache suppression is bundled here.
