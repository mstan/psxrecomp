# Overlay DLL Cache — Status (features + issues)

Living status for the overlay capture/compile/cache feature on
`feat/overlay-jit-cache`. Design rationale lives in
`overlay-recompilation-design.md`; this file tracks *what works* and *what's
broken*, with evidence.

## Working (verified on Tomba)

- **Capture** — on CD-DMA into game RAM, the runtime records the overlay bytes
  (full, data-inclusive) + executed PCs + dispatch-entry PCs, written to
  `overlay_captures.json` at clean shutdown. One region captured for Tomba
  village: `0x800E7000`, 356 KB, 15358 executed PCs, 66 dispatch seeds.
- **Evidence-scoped recompile (`--overlay`)** — `compile_overlays.py` runs the
  recompiler in entry-scoped mode (no whole-byte sweep). Set unconditionally
  for every overlay; not a human toggle.
- **Boundary-gated entry promotion** — a bare `DISPATCH_ENTRY` is promoted to a
  registered function only with boundary evidence (`jr $ra`-preceded, prologue,
  or region start). Dropped the data-as-code false entries: 122 → 88 functions,
  42 → 0 unsupported, 8 → 0 bad targets.
- **DLL cache** — clean 274 KB DLL emitted (`000E7000_B476006F.dll`). Loader
  lazily `LoadLibrary`s it on first dispatch into the dirty region, registers
  the 88 `func_XXXX` exports, dispatches them before the interpreter.
- **Config gate** — `[runtime] overlay_cache` (default off).
- **In-village behavior** — with the cache on, the village scene renders and
  plays correctly: the 88 registered functions execute natively, the deferred
  34 fall back to the interpreter, 0 dispatch misses, 0 exceptions.

## Issues

### OV-1 — Blue screen on village → overworld transition (FIXED 2026-06-07, Inc1-D)
**Resolution verified by mechanism, not just symptom.** With Inc1-D write-
invalidation, the village→overworld transition no longer softlocks. Counters at
the overworld: `invalidations=1` (`last_msg: invalidated region 0x000E7000
(+16384) on write 0x000E7388 -> 88 funcs`, the write from BIOS pc 0xBFC12210),
`unregistered_funcs=88`, and **`stale_blocked` climbing into the tens of
millions** — i.e. the game dispatches into the reused region constantly, and
every one of those (which pre-fix executed stale village native code = the blue
screen) is now blocked and routed to the interpreter. A/B confirmed earlier:
`overlay_cache=false` transitions cleanly. Verified one transition + free
roaming; broader scenes/games still untested.

**Note on benefit:** correctness only, so far. `dispatch_native≈122` vs
interpreter ≈31M — near-0% native, because (a) only one overlay is compiled and
(b) an invalidated region is not re-registered this session. Speed payoff needs
broad overlay coverage + Inc2 reload-on-return (CRC-matched re-registration).

Original root-cause analysis below, retained for the record.

### OV-1 (original analysis) — stale registration
**Symptom:** leaving the village to the overworld lands on a solid blue screen.
Not a crash: frame counter still advancing (8358→8364), `dispatch_miss_total=0`,
`in_exception=0`, GPU enabled and drawing. Execution cycles low BIOS/kernel
(`current_func≈0x650`, `last_store_pc=0xBFC11BF0`) — i.e. running but down a
wrong path / stuck in a BIOS wait.

**Root cause — stale registration.** The `0x800E7000` region is reused after
the village. Direct RAM-vs-captured comparison at the blue screen:

| offset | captured (what we compiled) | live RAM | verdict |
|---|---|---|---|
| `0xE73C0` (compiled func) | `27bdffb8` (addiu sp,sp,-0x48 prologue) | `0x800E92C4` ×4 (pointer table) | DIFFER |
| `0xE7000` (region start)  | zero-fill | `0x11111111` ×4 | DIFFER |
| `0xE8120` (compiled func) | real code | `0x801013F4` ×4 (pointers) | DIFFER |
| `0x100000` | — | — | MATCH |
| `0x13B000` | — | — | MATCH |

The **front of the region was overwritten** (now pointer tables, not our code)
while the **tail still matches** — and the loader **kept all 88 functions
registered** (`regions_checked=1`, loaded once, never re-evaluated; the
`already_checked` guard plus the absence of write-invalidation mean the
registration persists). The game then dispatches into `0xE7000` addresses whose
*content has changed*, hits our **stale village function pointers**, and runs
the wrong native code → wrong logic → blue.

**It is NOT:** a mid-function split (boundary-gated out; village renders fine),
nor a recompiler opcode gap (0 unsupported after the gate).

**It IS adjacent to** the DMA-time-vs-execution-time capture question
(design §2.2): the bytes changed after capture. But the correct fix is not
"capture different bytes" — it is "detect that the source bytes changed and
invalidate the registration."

**Fixes (in priority order):**
1. **Inc1-D — write-invalidation (primary).** Watch registered overlay pages;
   on any write into them, invalidate (unregister) the affected functions so
   the new content falls back to the interpreter / a re-evaluation. The
   RAM-write-path audit confirms a single store chokepoint with no bypasses, so
   this is sound; it needs the watched-page extension (current tracking is
   kernel-only). This directly prevents OV-1.
2. **Inc2 — registration-time + manifest CRC guard.** Before registering, verify
   the live region bytes still match the DLL's source CRC; refuse if not.
3. **Design §2.2 — execution-time capture.** Compile from the bytes that
   actually executed, not DMA-time bytes (defense in depth).
4. **Design §2.6 — segmentation.** The 356 KB region almost certainly merges
   the village overlay with other content; finer per-overlay segmentation +
   per-overlay cache keying reduces the blast radius of a partial overwrite.

**Confirming A/B (cheap, do next):** set `overlay_cache=false`, repeat the exact
village→overworld navigation. If it transitions cleanly with the cache off,
OV-1 is confirmed as cache stale-registration. If it still blue-screens, the
cause is upstream (interpreter or game-level) and this diagnosis is wrong.

## Implementation increments (see design doc §7)
- Inc1-A ✅ evidence-scoped `--overlay`
- Inc1-B ◐ boundary gate done; per-function audit + manifest pending
- Inc1-C ☐ loader registers only manifest entries
- Inc1-D ✅ write-invalidation — fixes OV-1 (verified)
- **Inc2 (next, biggest benefit lever):** reload-on-return — re-register a
  cached overlay when its region is reused and the bytes match, so a revisited
  overlay runs native again instead of staying interpreted after first exit.
