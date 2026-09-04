# Async background overlay compilation — implementation plan

> **SUPERSEDED IN PART — the sljit tier described below no longer exists.**
>
> This document was written while sljit was the toolchain-free fallback. That
> tier was replaced by tcc in `c2a3f6ad` (2026-06-25) and removed outright in
> `5b7e69b4` (2026-07-15), which deleted `lib/sljit/`, `overlay_sljit.{c,h}` and
> `overlay_compile_worker.{c,h}`.
>
> **The backend tiers today are `static → gcc → tcc`** (`docs/ARCHITECTURE.md`):
> gcc is the development default when it is on `PATH`, and TinyCC is bundled
> beside the shipped executable in `overlay_toolchain/` so players need no
> compiler. Both are external compilers invoked as a subprocess that produce a
> **DLL**; neither is an in-process JIT.
>
> That difference matters when reading this file: every passage below about
> serializing a `.sljit` blob, `overlay_sljit_deserialize`, `try_sljit_region`,
> `register_sljit_candidate`, or a per-fragment in-memory JIT registry describes
> a mechanism that was never how tcc works and no longer exists at all. Those
> passages are **not** transferable to the tcc tier by substituting the name.
> Read them as history. The async worker/file-handoff architecture, the
> gcc-versus-fallback mutual exclusivity, and the CPS work are still accurate.
>
> Current behaviour lives in `runtime/src/overlay_backend.c`,
> `runtime/src/code_provider.c` and `tools/compile_overlays.py`.

Status: **LARGELY ALREADY BUILT + VALIDATED under CPS (2026-06-18).** The "planned" feature below
turned out to mostly exist: `overlay_capture.c` (`overlay_autocapture_tick` — autocapture on
interp pressure, ON whenever the cache is on) + `autocompile.c` (background compile: probes for a
C compiler on PATH, spawns `cmd.exe /C <configured cmd>` on a watcher thread, applies on the emu
thread) + the `code_provider` abstraction (gcc vs sljit). It is FILE-BASED exactly as planned —
the worker writes DLLs; the dispatch thread loads them. Config-driven via `[runtime]
overlay_autocompile_cmd`.

What this session ADDED to make it work under CPS + match the user's gcc-only preference:
1. `compile_overlays.py --cps` (committed, §25.5) — the autocompile cmd passes `--cps`.
2. `overlay_loader.c overlay_loader_apply_live_policy` — when the active backend is gcc, sljit-live
   is forced OFF (gcc>interp; toolchain-less stays sljit>interp) — "mutually exclusive" per user.
3. The dev/soak config (`tomba_soak_a.toml`, untracked): `overlay_cache=true` +
   `overlay_autocompile_cmd = py -3 ../psxrecomp/tools/compile_overlays.py … --cps`. The PRODUCTION
   `game.toml` overlay_autocompile_cmd needs `--cps` appended when CPS ships to master.
   (Windows recipes must invoke the interpreter as `py -3`, never bare `python`: on machines where
   an MSYS2/Cygwin `usr/bin` precedes the Windows Python on PATH, `python` binds to the Cygwin
   build, which segfaults under the runtime's job-object spawn before writing any output — every
   compile fails with "(no output captured)". The runtime prints the resolved interpreter at
   startup and warns when it is a Cygwin binary.)

VALIDATED: autocapture fired (triggers≥1) → background `compile_overlays --cps` produced CPS
overlay DLLs (new CRCs, post-cache-clear) → loader loaded them (`loads`>0) → `dispatch_native`
rises, gcc box shows 0 sljit shards. ce_profile stays flat. As you PLAY, the cache auto-builds and
each scene goes native after its autocapture cycle (~60s cooldown) + compile.

REMAINING / FUTURE: broader capture coverage is just play-time (each scene's overlays auto-compile
on first sustained visit); optionally raise the autocapture cadence / cap; the in-process sljit
tier (toolchain-less users) is still leaf-only under CPS (call-fragments interp) until the
per-fragment entry-switch codegen lands. The DETAILED design below is retained as reference.

---

## (original plan — retained for reference; most is already implemented as above)

## 1. Goal (user's words)

> "I would like it to be (relatively) fast (via interp), and async compile in the background,
> and when it's in a healthy spot, the next time it's called, it finds the compiled shard and
> does that instead. So there shouldn't be visible user lag."
>
> "gcc for users that have it. for those who do not, sljit. Always async. if the user has gcc,
> no need to also do sljit. mutually exclusive basically."

Classic JIT tiering: **interp tier (immediate, no stall) → background compiler → native tier
(picked up on the next dispatch).** gcc and sljit are mutually exclusive backends, chosen by
toolchain availability. Always off the dispatch (guest) thread.

## 2. The measured problem this fixes

Title screen under CPS: `dispatch_interp_fallback ≈ 12.2M` vs `dispatch_native ≈ 14.8K` —
almost entirely interpreted, because the title's overlays are NOT gcc-compiled (only the kernel
+ the village `0xE7000` overlay were, from a short capture). Two slownesses:
1. **Sync compile hitch:** `try_sljit_region` (overlay_loader.c) JITs a region's shards
   *synchronously on the dispatch thread* on first miss → a visible spike.
2. **Steady interp:** call-heavy overlays decline the scoped sljit (§25.5) → interp forever, and
   uncompiled regions have no gcc DLL → interp.
In-game (village) feels fine because `0xE7000` is gcc-native. So: get overlays native, off-thread.

## 3. Architecture — FILE-BASED HANDOFF (the key safety property)

The worker produces ARTIFACT FILES (a `.dll` or a `.sljit` blob in the cache). The **dispatch
(guest-fiber) thread** does ALL candidate registration via the EXISTING load paths
(`try_load_region` for gcc DLLs, `overlay_sljit_deserialize` for sljit blobs). The worker NEVER
touches `s_cand` / `idx_*` / the sljit in-memory registry. This keeps the candidate table
single-threaded — no locks on the dispatch hot path, no torn reads.

```
guest fiber (dispatch)                     worker thread (background)
─────────────────────                      ──────────────────────────
overlay_loader_dispatch(addr):
  head<0 & in-window & not-done?
    → interp now (fast)                ──enqueue {phys, crc, snapshot}──▶ work queue
    → (try_load_region already                                          pop → compile:
       re-checks the cache each miss)                                     gcc:  compile_overlays pipeline → <crc>.dll
  next miss of same region:                                              sljit: JIT from SNAPSHOT → serialize <crc>.sljit
    try_load_region finds the new   ◀──────────── file in cache ───────  (atomic rename into place)
    .dll/.sljit → loads + registers
    (on THIS thread) → native
```

## 4. Components to build

### 4a. Toolchain probe (startup, once)
- gcc tier requires: the recompiler binary (`psxrecomp-game.exe`), a C compiler (`gcc`), and the
  `compile_overlays.py` driver + a Python interpreter — OR reimplement the driver in C (heavier).
  Probe: `gcc --version` exits 0 AND the recompiler + script paths exist.
- If gcc tier unavailable → sljit tier.
- Expose paths via a `[overlay_compile]` block in game.toml (recompiler, script, python,
  runtime_include, cache_dir) OR auto-derive relative to the detected project root (memcard_paths
  already resolves a project root via .git/CMakeLists.txt — reuse that). Shipped builds: no
  toolchain present → sljit tier automatically.
- Store the chosen tier in a global; the worker branches on it.

### 4b. On-miss enqueue (overlay_loader.c, overlay_loader_dispatch)
- Where: the `head < 0 && s_active && overlay_cache_window_contains(phys)` block, alongside the
  existing `try_load_region`. Replace the SYNCHRONOUS `try_sljit_region` with: enqueue the region
  for async compile + fall through to interp (return 0 → dirty_ram interp).
- Region key = the overlay CRC (per-variant; same model as the offline cache — same addr can be
  different code in different scenes → key by CRC, additive cache).
- Dedup: a per-CRC state map { QUEUED, COMPILING, DONE, FAILED }. Don't re-enqueue QUEUED/
  COMPILING/DONE. FAILED → backoff (retry after N misses, or never).
- Snapshot the overlay bytes AT ENQUEUE (memcpy the region out of guest RAM on the dispatch
  thread) so the worker never reads live guest RAM (avoids the RAM race). The existing capture
  (`overlay_capture.c` → overlay_captures.json) already snapshots — reuse/extend it.

### 4c. Worker thread
- One std::thread (or pthread) started at init when a tier is active. Condition-variable wait on
  the queue. Clean shutdown on exit (join; the runtime already has a shutdown path).
- gcc tier: write a SINGLE-region captures.json (from the snapshot) to a temp, run
  `python compile_overlays.py --captures <tmp> --cps --out-dir <cache> --recompiler ...
   --game-toml ... --runtime-include ...` (the `--cps` flag + codegen_ver=4 namespace already
  exist). The pipeline writes `<crc>.dll` into `cache/<game>/gcc/<arch-abi>/cg4/`. Mark DONE.
  - Incremental is built in (compile_overlays skips existing DLLs unless `--force`), so it's safe
    to run per-region or batched.
  - Atomicity: compile_overlays writes the .dll directly; if the loader could observe a partial
    file, compile to a temp + rename. (Check whether compile_overlays already does; if not, add a
    rename step — DLL load of a half-written file would be bad.)
- sljit tier: compile from the SNAPSHOT (not live RAM). NOTE the current
  `overlay_sljit_try_compile` reads cpu RAM (`fetch_word`) — add a variant that takes a byte
  buffer + base addr, and a SERIALIZE-ONLY mode (write the `.sljit` blob, do NOT call
  `register_sljit_candidate`). The dispatch thread deserializes + registers on next miss
  (`overlay_sljit_deserialize` path in overlay_loader.c). Confirm the sljit compiler instance is
  per-call (it is — `sljit_create_compiler` per fragment) so worker-thread use is safe.

### 4d. Loader pickup (mostly already works)
- `try_load_region(phys)` already scans the cache index and loads matching DLLs on miss → it will
  pick up worker-produced DLLs with no change, IF the cache index is refreshed. CHECK: does the
  loader cache its directory listing once at init, or re-scan on miss? If cached, add a
  "cache dirty" flag the worker sets (via an atomic) so the dispatch thread re-scans.
- For sljit blobs: ensure there's a deserialize-on-miss path keyed by CRC (the persist cache
  already deserializes at init — extend to deserialize-on-miss for blobs the worker just wrote).

## 5. Thread-safety checklist
- s_cand / idx_* / sljit in-memory registry: touched ONLY by the dispatch thread (file handoff).
- Guest RAM: worker reads ONLY the enqueue-time snapshot, never live RAM.
- Queue + per-CRC state map: guarded by one mutex (cold path — only on overlay miss + worker pop).
- File writes: atomic (temp + rename) so a partial artifact is never loaded.
- Worker lifetime: started after overlay_loader_init, joined on shutdown_runtime.
- The 4-second starvation watchdog: the worker is a separate thread, so a long gcc compile won't
  starve the guest fiber — but DON'T block the dispatch thread waiting on the worker.

## 6. Files
- `runtime/src/overlay_loader.c` — enqueue on miss (replace sync try_sljit_region), per-CRC state,
  cache re-scan flag.
- `runtime/src/overlay_compile_worker.c` (NEW) — the worker thread, queue, tier dispatch, subprocess
  spawn (gcc) / snapshot-sljit (sljit).
- `runtime/src/overlay_sljit.c` — add compile-from-buffer + serialize-only variant.
- `runtime/src/overlay_capture.c` — reuse/extend the snapshot for single-region captures.
- config_loader (+ game.toml) — optional `[overlay_compile]` paths; else auto-derive.
- Windows: subprocess via CreateProcess (or _popen); POSIX via fork/exec. There may already be a
  subprocess helper; otherwise keep it minimal + Windows-first (the dev target).

## 7. Validation
- Title screen: after a few seconds of background compile, `dispatch_interp_fallback` stops
  climbing and `dispatch_native` rises; the title visibly smooths out. No frame hitch at the
  moment of compile (it's off-thread).
- `ce_profile` stays FLAT (the leak fix must not regress).
- No crash through dwarf→overworld (the continuation-routing path) with overlays going native
  mid-scene.
- Toggle: force sljit tier (hide gcc) → still no lag, leaf shards native, calls interp.
- Shipped-like (no toolchain) → sljit tier auto-selected, no errors.

## 8. Risks / gotchas
- Running `compile_overlays.py` per-region is slow (Python + recompiler + gcc, seconds each).
  Batch or rate-limit; it's async so latency is OK, but don't spawn dozens concurrently — cap to
  1 in-flight compile.
- Per-CRC variant explosion (the §coverage-campaign long tail): many scene variants. Cap the
  queue; LRU-evict the per-CRC DONE set if needed (the on-disk cache is the source of truth).
- compile_overlays needs the SAME --cps + codegen_ver as the runtime (cg4). If they drift, the
  loader's codegen_ver check rejects the DLL → wasted compile. Keep PSX_OVERLAY_CODEGEN_VER in
  lockstep (it already gates both).
- The captured snapshot must match the live bytes the loader will CRC-check at load (cand_crc) —
  capture at the same moment / validate CRC before enqueue, else the DLL is rejected on load.
- Do not regress the committed CPS leak fix: everything here is additive + gated on the active
  tier; legacy (non-CPS) overlays still use the existing sync paths.
