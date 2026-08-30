# Testing

## Running the tests

```sh
cmake -S recompiler -B recompiler/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build recompiler/build
cd recompiler/build && ctest --output-on-failure
```

That is the whole thing. 38 tests, under 5 seconds, and it needs **no BIOS dump,
no disc image, and no generated code** — a plain recompiler build is enough. This
is the check to run before opening a PR.

Until 2026-07-27 no document in this repository mentioned `ctest`, `pytest`, or
how to run a test at all, so the suite was effectively invisible. If you add a
test, add it to `ctest` in the same commit — an unregistered test cannot fail,
and a test that cannot fail is not a test.

> On a memory-constrained machine, parallel builds of this tree can crash
> `cc1plus` while compiling the toml11-heavy `config_loader.cpp`. If
> `cmake --build` dies with no diagnostic, retry with `-j 2` or `-j 1`; the
> failure is resource exhaustion, not a code error.

### Running one test

```sh
ctest -R overlay_guard_codegen --output-on-failure   # by name (regex)
ctest -N                                             # list without running
```

Every test is also a plain executable or script, so you can run it directly:

```sh
./recompiler/build/bios_address_model_test
python recompiler/tests/test_overlay_guard_codegen.py \
       --recompiler "$(pwd)/recompiler/build/psxrecomp-game.exe"
```

> Pass the recompiler as an **absolute** path. A relative path satisfies the
> test's own `os.path.isfile` check but then fails inside `subprocess.run` on
> Windows with `WinError 2`, which looks like a broken test rather than a bad
> argument.

## What the suite covers

| Group | Where | Needs |
|---|---|---|
| C/C++ unit tests | `recompiler/tests/`, `runtime/tests/` | recompiler build |
| Codegen contract tests | `recompiler/tests/test_*.py` | `psxrecomp-game` |
| Runtime source-invariant guards | `runtime/tests/test_*.py` | nothing — they read source |

The source-invariant guards are the cheapest and most useful class here. They
assert structural properties of the runtime (an ordering holds, a fast path is
invalidated, a fallback exists) by reading the source, so they cost milliseconds
and catch whole regression classes without running a game. Registered from
`recompiler/CMakeLists.txt` rather than `runtime/CMakeLists.txt`, because the
runtime tree cannot configure until a BIOS has been generated, and these need
neither.

## Known-failing tests (not registered)

Three tests exist and are **deliberately left out of `ctest`** because they fail
today. They are not registered because a suite with a known-red test is a suite
people stop believing — the exact failure mode that took CI off pull requests in
the first place (see `.github/workflows/cli-release.yml`). Fix or retire them,
then wire them in.

| Test | Status |
|---|---|
| `runtime/tests/test_interpreter_perf_guards.py` | Asserts `psx_devices_mmio_sync` invalidates the inline cycle limit. It does not: the function delegates to `psx_devices_service_to_now()` (which clears `g_psx_cycle_fast_limit`, `psx_cycles.c:161`) **or** to `psx_devices_recompute_deadline()` (`:153-157`), and that second branch never clears it. Needs a timing owner to decide whether the guard found a real hole or the invariant moved. The guard is also partly stale — it still names `s_next_service_cycle`, since renamed to `psx_next_service_cycle`. |
| `runtime/tests/test_runtime_perf_diag_guards.py` | Asserts a substring that is no longer present in the runtime source. Either the diagnostic was removed or it was renamed; the guard has not been updated either way. |
| `runtime/tests/test_overlay_pair_dedup_runtime.py` | Needs its companion harness (`overlay_pair_dedup_harness.c`) built. Unlike the other Python tests it is not source-only, so it needs a build target before it can be registered. |

## Tests that are not in `ctest` and should not be

`runtime/tests/` also holds fixtures and harnesses (`*_fixture.c`, `*_harness.c`)
that are inputs to other tests, not tests themselves. Do not register them.

Several C tests under `runtime/tests/` require a **built runtime**, which
requires a generated BIOS (see [`BUILDING.md`](BUILDING.md)). Those are wired
into `runtime/CMakeLists.txt` and run from `runtime/build`:

```sh
cd runtime/build && ctest --output-on-failure
```

## CI

`.github/workflows/cli-release.yml` runs on `workflow_dispatch` and published
releases only. Its header explains why per-PR triggers were removed on
2026-07-25: the Windows job failed often enough that a red check stopped
carrying information, and a check nobody trusts costs attention without buying
confidence.

That reasoning still holds. The gap it left was that no fast, trustworthy
alternative existed. The `ctest` suite above is a candidate: it is hermetic
(no BIOS, no disc, no network), takes under five seconds, and is currently
green. Restoring a per-PR check on top of it is a smaller decision than
restoring the old one.
