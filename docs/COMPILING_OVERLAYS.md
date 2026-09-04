# Compiling overlays from `overlay_captures.json`

**Audience:** developers preparing a release who want to ship as much
native-compiled overlay coverage as possible, plus modders and players who want
to pre-build coverage for their own machine.

> **What `overlay_captures.json` is.** PS1 games stream *overlays* — chunks of
> code loaded from disc into RAM at runtime and later overwritten. The static
> recompiler never saw them at build time, so on first visit they run on the
> dirty-RAM interpreter (correct, but slower). With `[runtime] overlay_cache =
> true`, the runtime records each overlay it sees (bytes + executed PCs) into
> `overlay_captures.json` next to the exe. Compiling that file turns those
> overlays into native code.
>
> ⚠️ **Privacy:** `overlay_captures.json` contains snapshots of the game's own
> code read from *your* disc. Keep it private — do not post it publicly.

§0 frames the production goal; §1 builds the gcc cache you ship; §2 bakes
overlays straight into the executable.

---

## 0. The production goal: ship a full gcc cache

**A good release ships as many developer-built gcc shards as possible.** gcc
produces the best-optimized native code, and a shard the developer already built
runs at full speed on the player's *first* visit to that area — no interpreter
hitch, no wait for a background compile. So the intended workflow is:

1. Play through the game (or the areas you care about) on a dev box with
   **gcc + Python**, with `overlay_cache = true`, to accumulate
   `overlay_captures.json` coverage.
2. Pre-build the gcc cache from it (§1) and ship that `cache/` folder in the
   release.

**The runtime auto-compile is a fallback that fills the gaps, not the plan.**
With `overlay_cache = true`, if a player reaches an area your shipped cache
doesn't cover, the runtime compiles it in the background so the *next* visit is
native. On a dev box that background compile uses gcc; on a **player box with no
toolchain**, it falls back to the fully self-contained bundle beside the exe
(`overlay_toolchain/`: embedded Python + **TinyCC** + the recompiler + this
script + headers). tcc keeps such players from ever being stuck on the
interpreter, but it optimizes worse than gcc — which is exactly why you want the
developer's gcc shards to cover as much as possible up front.

The rest of this doc is how you build that cache (§1) or bake overlays straight
into the binary (§2).

---

## 1. Build the gcc cache (the shard you ship)

This produces the `cache/…/*.dll` shards you ship in the release, ready from the
first launch. It's the same cache the runtime would eventually build in the
background — but done up front, with gcc, by you.

```sh
python psxrecomp/tools/compile_overlays.py \
    --captures        <exe-dir>/overlay_captures.json \
    --game-toml       game.toml \
    --recompiler      psxrecomp/recompiler/build/psxrecomp-game.exe \
    --runtime-include psxrecomp/runtime/include \
    --out-dir         <exe-dir>/cache \
    --gcc             C:/msys64/mingw64/bin/gcc.exe \
    --cps
```

Then just launch the game — the loader rescans the cache dir and picks the
shards up automatically.

**The flags that matter:**

| Flag | Why |
|------|-----|
| `--captures` | The `overlay_captures.json` to read. If the runtime set `PSX_OVERLAY_CAPTURES`, that wins; for a manual run pass this explicitly. |
| `--game-toml` | Reads the game id (used in the cache path) and the `[widescreen]` site lists. |
| `--recompiler` | `psxrecomp-game.exe`. **Must be built from the same source tree** — a stale binary is rejected up front (see "Staleness guard"). |
| `--runtime-include` | The runtime `include/` dir. Supplies the codegen version + hash that namespace the cache. Also supplies the default `--project-root`. |
| `--project-root` | The root the recompiler resolves the BIOS profile against. Defaults to the framework root derived from `--runtime-include`, which is right for every normal invocation — pass it only if that derivation is wrong. See "Packaged configs" below. |
| `--out-dir` | Cache root. Point it at the `cache` folder next to the exe. |
| `--gcc` | Absolute path to your mingw gcc, e.g. `C:/msys64/mingw64/bin/gcc.exe`. gcc gives the best-optimized shards. |
| `--cps` | **Required** so the generated code matches a CPS runtime build. If the runtime is CPS and you omit this, the DLLs misbehave. Match the runtime. |
| `--compiler tcc` | Use the bundled TinyCC instead of gcc (toolchain-free). tcc shards land in their own `tcc/` cache namespace. |
| `--jobs N` | Parallel workers (default: CPU cores − 2). Regions compile independently, so this is a large speedup on a full capture set. `--jobs 1` forces the sequential path. |
| `--force` | Rebuild shards even if they already exist. Normally the compile is **incremental** — it skips any DLL already present and any function already covered by an existing shard. |

**Where the shards land.** The cache is namespaced so different games, compilers,
CPU architectures, and codegen versions never mix:

```
<out-dir>/<game-id>/<gcc|tcc>/<os>-<arch>/cg<N>_<hash>/<phys>_<crc>.dll
                                                        <phys>_<crc>.ranges
```

e.g. `cache/SLUS-01395/gcc/win-x64/cg7_1a2b3c4d/000E7000_B476006F.dll`. The
loader computes the exact same path, so if you build into the right game's cache
folder it just works. (If you build with a recompiler/headers from a *different*
codegen version, the shards land in a different `cg…` folder and the current
runtime ignores them — that's the version guard working, not a bug.)

---

## 2. Bake overlays into the executable (`--static`)

This is the literal "compile them **along with** the executable": instead of
per-area DLLs loaded at runtime, every captured overlay is emitted into a single
C file that is compiled and linked **into the runtime binary**.

```sh
python psxrecomp/tools/compile_overlays.py \
    --static \
    --captures        <exe-dir>/overlay_captures.json \
    --game-toml       game.toml \
    --recompiler      psxrecomp/recompiler/build/psxrecomp-game.exe \
    --runtime-include psxrecomp/runtime/include \
    --out-dir         generated/ \
    --gcc             C:/msys64/mingw64/bin/gcc.exe \
    --cps
```

Output: `generated/overlays_static.c` — the auto-generated, content-validated
`psx_overlay_dispatch()` plus an extern prototype for every compiled entry —
and **one translation unit per overlay** beside it, `overlays_static_0000.c`,
`overlays_static_0001.c`, … (each carries its own `#include`s and a private
symbol namespace, so it compiles standalone). Point the runtime build at
`overlays_static.c` (`GAME_OVERLAY_STATIC_C` in `psxrecomp_add_game_runtime`);
`runtime.cmake` globs the sibling parts with `CONFIGURE_DEPENDS`, so a run that
changes the part count needs no reconfigure. No `cache/` folder and no DLL
loading are involved at runtime.

Why split: a whole title's overlays make a single C file of hundreds of MB
(BoF3: 309 MB, 8.6 M lines), which one `gcc` invocation compiles on one core.
Per-overlay units build across every core, and because the tool only rewrites
a part whose content changed, an incremental rebuild recompiles only the
overlays that changed. `--static-single-file` restores the monolithic output.

The tool side parallelizes too: `--jobs N` (default cores−2) runs each
capture's recompile + audit in a process pool and merges results in capture
order, so the emitted C is byte-identical to `--jobs 1`.

**Trade-off vs. the DLL cache:** `--static` is a fixed snapshot chosen at build
time — it will *not* grow as the player explores new areas. The DLL cache (§0/§1)
is the mechanism that keeps improving with play. Use `--static` when you want a
single self-contained binary with a known coverage set baked in; use the DLL
cache for everything else.

---

## Staleness guard (read this if it refuses to run)

The recompiler *binary* and the codegen *source* are tied together by a hash.
Before compiling anything, the script runs `psxrecomp-game.exe --codegen-hash`
and compares it to the hash baked into `--runtime-include`. If they differ — or
the binary is too old to support the flag — it **aborts** rather than silently
emitting shards with stale codegen (a real bug class: read-tag matches,
semantics are old). The fix is always the same:

```sh
cmake --build psxrecomp/recompiler/build --target psxrecomp-game
```

Rebuild the recompiler from the current tree, then re-run.

---

## Packaged configs, and `FATAL: no BIOS profile found`

If every shard fails with

```
psxrecomp-game: FATAL: no BIOS profile found. Searched '<dir>' for
bios/SCPH1001.toml and <framework>/bios/SCPH1001.toml. ...
```

then the recompiler could not locate a BIOS profile, and the `<dir>` in the
message is where it looked. It needs one because the profile supplies the BIOS
address model that codegen folds jump tables through — there are no built-in
windows.

Two things make this easy to hit, and neither is a mistake on your part:

1. **We spawn the recompiler with `cwd = dirname(game.toml)`,** because the
   seeds and capture paths in the config are relative to it. You cannot
   override that cwd from the command line.
2. **`--ws-config` deliberately does not adopt the config's paths,** so a
   `[recompiler] bios_config` in your `game.toml` is *not* consulted for
   overlay compiles — only the `[widescreen]` site lists are.

Together those mean the profile is resolved by probing a directory, and for a
**packaged** config (`packaging/release/game.toml`) that directory contains
neither `bios/SCPH1001.toml` nor a vendored framework one level down. Every
shard then dies and the cache silently fails to build — the run still exits,
so the only symptom is that the game ships with no cache and every player's
first session runs interpreted. This was [issue #72][i72].

`--project-root` is the fix, and it **defaults to the framework root derived
from `--runtime-include`**, so a normal invocation needs nothing new. Pass it
explicitly only when that derivation is wrong:

```sh
python psxrecomp/tools/compile_overlays.py \
    --game-toml       packaging/release/game.toml \
    --recompiler      psxrecomp/recompiler/build/psxrecomp-game.exe \
    --runtime-include psxrecomp/runtime/include \
    --project-root    psxrecomp \
    --out-dir         <exe-dir>/cache \
    --gcc             C:/msys64/mingw64/bin/gcc.exe
```

Building against the packaged `game.toml` is **required**, not optional: the
config half of the cache tag is derived from it, so a cache built against the
dev config lands under a different tag and the shipped runtime will not read
it. `--project-root` is what lets you do that without also having to move the
BIOS profile.

**Is the cache actually being USED?** Shards on disk prove nothing — the loader
can reject them. `tools/stall_report.py` answers that, plus interpreter
attribution, in one command against a running title:

```sh
py -3 tools/stall_report.py --port <debug port> snap            # since boot
py -3 tools/stall_report.py --port <debug port> run --secs 60   # windowed
```

It is a passive ring consumer: no arming, no pausing, no stepping. It reads
cumulative counters (two snapshots → a window by subtraction) and the always-on
PC-sample ring, so you can attach long after the interesting thing happened.
`dispatch_native` vs `dispatch_interp_fallback` is the verdict; the
instructions-per-dispatcher-round-trip ratio tells you whether interpreted code
is chaining locally or thrashing per-block.

**Verify, don't assume.** Run the preflight and read the result line:

```sh
python psxrecomp/tools/compile_overlays.py ... --check
```

`--check` builds every shard into a throwaway temp dir, never touches the real
cache, and exits non-zero if any shard that should build fails. Watch for
`PSX_SHARD_RESULT ok=N failed=M skipped=K`. The runtime also parses that line
out of the provider's output, so `autocompile_status` reports
`shard_ok` / `shard_fail` / `shard_skipped` over the TCP debug server — a
shard-compile failure is visible in-game, not only in the provider's console.

[i72]: https://github.com/mstan/psxrecomp/issues/72

---

## Quick reference

- **I'm shipping a release:** play to accumulate coverage, then pre-build the gcc
  cache into `<exe>/cache` and ship it — the more you cover, the less the player
  ever falls back to tcc or the interpreter (§0/§1).
- **I want the overlays inside the .exe itself:** `--static` → `overlays_static.c`
  linked into the runtime (§2).
- **What about areas I didn't cover?** `overlay_cache = true` lets the runtime
  fill the gaps on the player's box (gcc if present, else bundled tcc) — a
  fallback, not a substitute for shipping gcc shards (§0).
- **It won't compile / rejects the recompiler:** rebuild `psxrecomp-game`
  (staleness guard).
- **The shards build but the runtime ignores them:** check the `cg<N>_<hash>` and
  `<os>-<arch>` folder match the runtime you're launching, and that `--cps`
  matched the runtime build.

Design/rationale for the whole system lives in
`internal/overlay-recompilation-design.md` and `internal/overlay-plan.md`;
`FEATURES.md` is in this folder.
