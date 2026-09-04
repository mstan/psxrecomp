# Overlay System — Implementation Plan

**Written:** 2026-05-30  
**Status:** Active

---

## Goal

Make PSX overlay code transparent to the user. Shipped builds run pre-compiled
overlays at native speed (Layer B). Unknown overlays fall back to the interpreter
once, compile in the background, and run as native code from the second encounter
onward (Layer A). Users contribute new coverage automatically by playing.

---

## Data structures

### Capture set

A hash map keyed by `load_addr`. Write-once: a given load address is inserted
exactly once regardless of how many times that overlay loads during a session.

```c
typedef enum {
    OVERLAY_STATE_QUEUED    = 0,  /* in compilation queue, not yet started */
    OVERLAY_STATE_COMPILING = 1,  /* background thread is working on it    */
    OVERLAY_STATE_COMPILED  = 2,  /* DLL written to cache, dispatch patched */
} OverlayState;

typedef struct OverlayEntry {
    uint32_t      load_addr;
    uint32_t      size;
    uint8_t      *bytes;          /* heap copy of the DMA'd bytes          */
    OverlayState  state;          /* atomic, guards queue dedup            */
    struct OverlayEntry *next;    /* intrusive hash chain                  */
} OverlayEntry;
```

### Compilation queue

A simple FIFO of `OverlayEntry *` pointers into the capture set. Does not own
data. Background thread drains it. Never contains duplicates — the state machine
on the capture set entry prevents duplicate pushes.

---

## State machine (per entry)

```
DMA completion (first time, load_addr not in set)
  → alloc entry, copy bytes
  → state = QUEUED
  → push pointer onto queue

DMA completion (load_addr already in set)
  → state == QUEUED    → no-op  (already waiting to compile)
  → state == COMPILING → no-op  (being compiled now, interp handles it)
  → state == COMPILED  → no-op  (dispatch table has it, interp won't fire)

Background thread (dequeues entry)
  → state = COMPILING
  → emit C for bytes at load_addr
  → compile to DLL → write cache/<game_id>/<hash>.dll
  → LoadLibrary + register functions in live dispatch table
  → state = COMPILED
```

The `NEW→QUEUED` transition and the queue push are done under a single mutex
lock so a concurrent DMA completion on a second thread cannot push the same
entry twice.

---

## Implementation phases

### B-1 — Capture + JSON dump  *(prerequisite for everything)*

**Files:** `runtime/src/overlay_capture.c` + `runtime/include/overlay_capture.h`

- `overlay_capture_init(const char *game_id)` — called at game handoff
- `overlay_capture_on_dma(uint32_t load_addr, uint32_t size, const uint8_t *bytes)` — called from `dma.c` `execute_ch3_cdrom` when `load_start < 0x1C0000`
- `overlay_capture_write_json(const char *path)` — writes `overlay_captures.json` next to the executable; called from `main.cpp` at clean exit
- JSON schema per entry:
  ```json
  { "load_addr": "0xA3000", "size": 208896, "bytes_b64": "...", "seeds": [] }
  ```
  (`seeds` is empty at capture time; filled in by the ingestion script from per_pc data)

**Hook in `dma.c`:**  existing `execute_ch3_cdrom` already has `load_start` and
`total_words`. Add one call after the existing cd_dma_log push:
```c
if (!step && load_start < 0x1C0000u)
    overlay_capture_on_dma(load_start, total_words * 4u, ram + load_start);
```

**Hook in `main.cpp`:**  at clean shutdown (SDL quit, window close) call
`overlay_capture_write_json`. Retail and dev builds both do this.

---

### B-2 — psxrecomp-game overlay compilation

**Files:** `recompiler/src/overlay_compiler.cpp`, extended `game.toml` schema

`game.toml` gains an `[[overlays]]` array:
```toml
[[overlays]]
load_addr  = "0xA3000"
capture    = "captures/A3000.bin"
seeds      = ["0xA3140", "0xA52C0"]
```

`psxrecomp-game` reads each entry, disassembles the MIPS bytes at `load_addr`,
traces from seeds, emits C functions, and appends to the game's dispatch table.
These functions are linked into the shipped binary at zero runtime cost.

---

### B-3 — Ingestion script

**File:** `tools/ingest_overlays.py`

```
python3 tools/ingest_overlays.py \
    --captures overlay_captures.json \
    --per-pc   logs/SCUS-94236/per_pc.json \
    --out-dir  captures/ \
    --toml     game.toml
```

- Reads `overlay_captures.json` (from user submission or own playthrough)
- Reads `per_pc.json` for executed addresses within each overlay range
- Writes `captures/<load_addr_hex>.bin`
- Appends new `[[overlays]]` blocks to `game.toml` (skips already-present entries)

After running, `psxrecomp-game --config game.toml` re-emits the dispatch table
with the new overlays compiled in.

---

### A-1 — Cache DLL check on DMA completion

**Files:** `runtime/src/overlay_capture.c` (extended)

On DMA completion, after the capture-set insert:
- Compute `hash = crc32(bytes, size)`
- Check `cache/<game_id>/<hash>.dll` exists
- If yes: `LoadLibrary` → enumerate exported functions → register each in the
  live dispatch table for `[load_addr, load_addr + size)` → set state COMPILED
- If no: proceed to queue for background compilation (A-2)

---

### A-2 — Background compiler thread

**Files:** `runtime/src/overlay_compiler_thread.c`

A single long-lived thread started at game handoff:

```
loop:
  wait on queue semaphore
  dequeue OverlayEntry*
  set state = COMPILING
  emit_c_for_overlay(entry)        // calls into recompiler lib
  compile_c_to_dll(entry)          // invokes GCC/Clang subprocess
  LoadLibrary + register dispatch
  set state = COMPILED
```

The recompiler is embedded in the runtime as a static library for dev builds.
For retail builds, the compiler subprocess path uses the system C compiler if
present, or skips to "write JSON only" mode if not (Layer A gracefully degrades
to JSON-only on systems without a toolchain).

---

## Build order

1. **B-1** — capture set + JSON dump (unblocks everything)
2. **B-2** — `psxrecomp-game` overlay compilation (unblocks B-3)
3. **B-3** — ingestion script (completes the contribution loop)
4. **A-1** — cache DLL check (fast path for returning users)
5. **A-2** — background compiler thread (self-healing on first run)

B-1 through B-3 are useful independently: they close the developer feedback
loop and give users a way to contribute without any runtime compilation.
A-1 and A-2 are the quality-of-life layer.

---

## What already exists

| Component | File | Status |
|---|---|---|
| Dirty-RAM interpreter (last-resort fallback) | `runtime/src/dirty_ram_interp.c` | Done |
| CD DMA ring log (`cd_read_log` TCP cmd) | `runtime/src/dma.c` | Done (filter + 65536 ring, 2026-05-30) |
| Dispatch table infrastructure | `runtime/src/full_function_emitter.cpp` | Done |
| Recompiler core (MIPS→C) | `recompiler/src/` | Done |
| `game.toml` config loader | `recompiler/src/config_loader.cpp` | Done |
