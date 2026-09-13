# Draft: opt-in runtime IPO/LTO validation

Status: experimental, OFF by default. Tracking: `beads-eio.3.149`.
The PE report is supplemental evidence, not a locally reproduced benchmark.
No IRQ, device deadline, I-cache, guest codegen or overlay ABI behavior changes.

## Build contract

`-DPSX_RUNTIME_IPO=ON` checks C and C++ IPO support and enables it only on runtime
targets, for Release, RelWithDebInfo and MinSizeRel. Debug and custom configurations
are not enabled by this option. Unsupported toolchains fail with an explicit
diagnostic rather than silently ignoring a requested experiment.

The default OFF path changes no target property, including an explicit parent
project IPO policy. This is not a project-wide
`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`: third-party libraries, emitters and other
targets are not opted in. This narrower scope is deliberate for validation and
may not reproduce the contributor's project-wide result.

CMake provides the target/configuration properties and support probe used here:
[IPO target property](https://cmake.org/cmake/help/latest/prop_tgt/INTERPROCEDURAL_OPTIMIZATION.html),
[CheckIPOSupported](https://cmake.org/cmake/help/latest/module/CheckIPOSupported.html).

## PGO is a separate variable

The existing `pgo-train` command configures generate/debug-ON, builds, trains,
then configures use/debug-OFF and builds again. It reports failures. The supplied
early-stop report does not match that successful control path; do not change
the pipeline without the failing revision, command and exit/error evidence.

Use the existing `--cmake-extra` interface to pass the IPO option to both PGO
build phases if testing the combination. Inspect actual compile/link commands;
an ON cache entry alone does not prove every target was built with IPO.

Runtime CMake PGO and this IPO option affect linked runtime/generated C/C++.
`compile_overlays.py` launches a separate `-shared -O2` compiler command with
neither profile flags nor IPO. They do not directly profile/optimize PE's dynamic
decoder DLL. Gains may come from linked code or callbacks, which must be measured.

Do not replace a user's working profiles or cache when running experiments.
Use isolated project/build/save directories and record profile identities.

## Automated checks

- Default OFF leaves target properties unset; parent IPO choice preserved.
- Injected unsupported toolchain fails only when IPO is requested.
- Real supported-toolchain two-file C/C++ IPO build/link/execution.
- Debug compilation emits no IPO flags; unrelated target remains unchanged.
- Mocked PGO success/failure orchestration and direct overlay compiler command.
  These mocks do not validate real profile collection or performance.

The real IPO smoke check explicitly reports a skip when the tested compiler
cannot support IPO; that does not count as a positive link/run validation.

## Evidence needed before a default-on or performance promotion

1. Repeated content-aligned LTO off/on trials with the same PGO profile, compiler,
   debug flags, assets, input and cache state. LTO-only/full-factorial trials can
   answer broader attribution questions, but are not required merely to measure
   LTO's incremental effect with one fixed profile.
2. Per-frame tails/maxima and audio underruns, not only periodic average FPS.
   The reported small incremental gain overlaps the earlier run-to-run spread.
3. Regenerated title regression: Tomba, MMX6 and Ape, including LLE boot, FMV,
   menus and gameplay. Do not infer unchanged behavior from frame counts alone.
4. Shipping compiler/linker/platform and launcher-enabled coverage, with profile
   mismatch/missing-function warnings visible during profile validation. Record
   binary size, build/link time and peak memory as well as host performance.
5. Exact PE configuration, profile/capture identities and raw measurements from
   the contributor. Their project-wide IPO result is not a guarantee for this
   narrower runtime-only option or other titles.

## Independent merge review

The opt-in path was rebuilt with Clang 22.1.8 and ThinLTO, then exercised for
more than 11,000 frames each in Tomba, Mega Man X6 and Ape Escape. All three
runs completed normally with LLE boot, native overlay dispatch, nonzero SPU
output, zero audio underruns and no kernel-state mismatch; representative
screenshots also showed normal output. The automated IPO and mocked PGO checks
above passed in the same review.

This establishes a reasonable correctness baseline for merging the experimental
option while it remains OFF by default. It does not establish a performance win,
validate real PGO profile collection, or justify enabling IPO by default. The
remaining evidence above is still required for either claim.
