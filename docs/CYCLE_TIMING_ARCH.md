# Cycle-driven CPU/peripheral timing — architecture

Status: design proposal. No code committed against this doc yet.

## Why

The runtime currently has no real notion of PSX wall-time. SIO byte
readiness, ACK timing, IRQ delivery, and VBlank scheduling all infer
"time" from proxies: MMIO access count, dispatch_count heuristics, host
wall-clock, or function-boundary callbacks. Every memory-card debugging
detour this branch has accumulated — burst drains, hard-cap defers,
SELECT-lifetime owner guards, hybrid sync/async TX paths — has been a
symptom of the same missing primitive: there is no `psx_cycle_count`.

Real PSX hardware advances peripherals on every CPU cycle. SIO TX_RDY
clears for ~1088 cycles per byte regardless of whether the CPU is in an
exception. ACK arrives ~170 cycles after byte completion. VBlank fires
on the GPU's own cycle deadline. The CPU is one client of the bus, not
the master of time.

Until the runtime models that, every fix to the symptom (pad leaks,
abort_reselect, etc.) is structurally a guess.

## Architectural target

### 1. Central PSX cycle clock

```c
extern uint64_t psx_cycle_count;
```

- Owned by a new `psx_cycles.c` / `psx_cycles.h`.
- Monotonically increasing. Never decremented.
- Advanced by `psx_advance_cycles(int cycles)` (signed int for symmetry
  with sio_tick(int)).
- All peripherals derive their schedules from this clock.

### 2. Peripheral scheduler

```c
void psx_advance_cycles(int cycles);
```

Single entry-point. Internally:
1. `psx_cycle_count += cycles`
2. Call `sio_advance(cycles)` — shifter/buffer/ACK timing
3. Call `gpu_advance(cycles)` — VBlank deadline
4. Call `timers_advance(cycles)` — root counters (later)
5. Call `cdrom_advance(cycles)` — CD timing (later)

Each peripheral exposes its own `*_advance(int cycles)`. The dispatcher
holds no peripheral knowledge. Order matters only when one peripheral's
advance can fire an IRQ that affects another's deadline computation —
not the case for the initial slice.

**No `in_exception` gating on hardware time.** Real PSX SIO keeps
shifting while the CPU is in the exception handler. Our peripheral
advance must reflect that.

### 3. Interrupt correctness — separate "pending" from "entry"

Two distinct concepts:

- **IRQ source pending** = `i_stat & i_mask != 0`. Set by peripherals
  any time their schedule fires. Independent of CPU mode.
- **CPU may enter exception** = `(SR & 0x401) == 0x401` AND not already
  in a kernel exception frame AND we're at a legal instruction
  boundary.

`psx_check_interrupts(cpu)` becomes:

```c
void psx_check_interrupts(CPUState* cpu) {
    /* hardware time always advances */
    psx_advance_cycles(<block cycles since last check>);

    /* peripheral writes to i_stat already happened in *_advance() */

    /* CPU-side gate: are we at a legal entry point? */
    if (!cpu_can_take_interrupt(cpu)) return;
    if ((i_stat & i_mask) == 0) return;

    /* Enter exception */
    cpu_enter_exception(cpu);
}
```

`cpu_can_take_interrupt(cpu)` checks:
- `cpu->cop0[SR] & IEc` (bit 0)
- `cpu->cop0[SR] & IM[2]` (bit 10)
- not currently inside a previously-entered exception (use `in_exception`
  ONLY as a re-entry guard against host-side recursion through
  `psx_dispatch` from inside the BIOS handler — not as a hardware-time
  gate)

The handler may now run for a long time without freezing peripheral
schedules. SIO can fire its ACK IRQ while the CPU is mid-handler; the
new IRQ pends in `i_stat` and is delivered on the next legal CPU entry
point after the current exception returns.

### 4. SIO model — single bus, no device-specific paths

SIO0 has one shifter, one TX buffer, one ACK pipeline. Card and pad
share it. Modeling:

```c
struct sio_bus {
    int      shift_active;
    uint8_t  shift_byte;
    int      shift_remaining_cycles;     /* counts down each advance */
    int      tx_buffered;                /* per no$psx, single byte */
    uint8_t  tx_buffer;
    int      ack_remaining_cycles;       /* fires IRQ when reaches 0 */
    int      ack_pending;
};
```

`sio_write(SIO_TX_DATA)`:
- if `!shift_active`: load shifter, clear TX_RDY/TX_EMPTY
- else if `!tx_buffered`: load buffer, clear TX_EMPTY (TX_RDY already 0)
- else: byte dropped (per no$psx — only the FIRST write while busy
  buffers; subsequent writes ignored)

`sio_advance(cycles)`:
- if `shift_active`: `shift_remaining -= cycles`. On <= 0: run state
  machine on the shifted byte (`sio_process_byte`), schedule ACK, promote
  buffer if any, else restore TX_RDY.
- if `ack_pending`: `ack_remaining -= cycles`. On <= 0: set
  `i_stat |= 1<<7` (gated by `SIO_CTRL.ACK_IRQ_EN`).

`sio_read(SIO_STAT)`:
- Returns current shifter/buffer/ACK state. No tick on read.

Card and pad protocols both use this single mechanism. There is **no**
"card busy → block pad" check anywhere. The natural timing of TX_RDY
and TX_EMPTY enforces mutual exclusion via the existing BIOS pad-poll
behavior — pad's TX writes during card-busy go to the buffer or are
dropped; their `mc_state` mutation does not happen because the byte is
held in the SIO buffer until the card's shift completes, at which point
the buffered byte becomes the next byte processed against `mc_state` in
its current state. **If real hardware would have lost the pad poll's
byte**, our model loses it. The pad chain handler retries on next
VBlank.

### 5. Recompiler integration — block-level cycle accounting

Each emitted basic block:
- gets a precomputed `block_cycles` count (sum of per-instruction
  cycles for the block's MIPS instructions; integer multiplication and
  load-delay penalties accounted for)
- emits `psx_advance_cycles(<block_cycles>);` before its `psx_dispatch`
  return

Block boundaries that contain MMIO reads — **option A required** per
explicit user direction:
- Split the block at every MMIO read. Cycles up to that point are
  advanced before the read so SIO_STAT (and analogous registers) show
  the elapsed guest time the BIOS expects.
- The simpler "advance the whole block before the read" was rejected
  because it over-advances and may cause the BIOS to see a TX_RDY
  transition earlier than the real bytes have actually shifted.

For MMIO writes, advance the block's accumulated cycles before the
write so peripherals see the same elapsed time the CPU sees.

Busy-wait loops (e.g., `while (!(STAT & TX_RDY));`) become tight loops
of small basic blocks. Each loop iteration advances cycles. After
~1088 cycles total, the SIO shifter completes, TX_RDY restores, the
loop exits. This is the architectural fix for the access-paced
`sio_tick` problem.

### 6. VBlank — cycle-based deadline

```c
static uint64_t next_vblank_deadline_cycles;

void gpu_advance(int cycles) {
    if (psx_cycle_count >= next_vblank_deadline_cycles) {
        next_vblank_deadline_cycles += NTSC_FRAME_CYCLES;  /* ~33,868,800 / 60 */
        i_stat |= (1 << IRQ_VBLANK);
        gpu_lcf ^= 1;
        gpu_vblank_callback();  /* SDL present + debug_server_poll */
    }
}
```

`dispatch_count` is removed from the VBlank path. SDL/TCP polling
still happens on VBlank fire — the host-side cadence is determined by
how fast the recompiled code runs, but that's correct: VBlank fires at
the right PSX-visible cycle count regardless of host wall-clock.

### 7. Migration strategy

Vertical-slice approach. Each step independently testable, mergeable.

**1.0e-a: cycle counter scaffold**
- Add `psx_cycles.c/.h` with `psx_cycle_count` and
  `psx_advance_cycles()`. Empty body except for the count update.
- All peripherals' `*_advance(cycles)` declared but no-op stubs.
- No recompiler integration yet.
- Validation: zero behavior change; build clean; baseline preserved.

**1.0e-b: SIO scheduler under cycle clock**
- Implement `sio_advance(cycles)` driven from
  `psx_advance_cycles`. SIO state vars (already on tree from 1.0c-v2)
  become live.
- TX_DATA writes feed the shifter; sio_process_byte called from
  shift-complete; ACK fires from cycle countdown.
- Critical: `psx_advance_cycles` is called from a SINGLE site
  initially — `psx_check_interrupts` — using a fixed estimate (e.g.,
  `BLOCK_CYCLES_AVG=8` per call). This isn't accurate per-block but
  proves the time-source plumbing works without recompiler changes.
- Validation: pad polling at shell still works; card protocol behavior
  may change.

**1.0e-c: VBlank cycle-based**
- `gpu_advance` fires VBlank from cycle deadline.
- Remove `dispatch_count` from interrupts.c. Keep
  `!card_active || progress_stale` defer logic OUT — it becomes
  obsolete once timing is correct.
- Validation: TCP still responsive; VBlank rate tracks PSX-visible
  cycle-time.

**1.0e-d: per-block cycle accounting in recompiler**
- Recompiler emits `psx_advance_cycles(<block_cycles>)` per block.
- Replace the fixed `BLOCK_CYCLES_AVG` from 1.0e-b with per-block.
- Validation: accurate timing; SIO bytes shift in cycle-correct
  windows; card txns no longer receive pad TX during READ_DATA.

**1.0e-e: card-read full-sector validation**
- Confirm card transactions reach 137 bytes.
- `[0x72F0]` reaches 0x80.
- `mc_read_done > 0`.
- Beetle byte-trace cross-check.
- Memory-card screen renders directory entries.

## Open questions to resolve before 1.0e-a

1. **What's the average cycles-per-block for our recompiled BIOS?**
   Need this for 1.0e-b. Pick the wrong number and pad polling rate is
   off, card-shift duration is wrong. Estimate: profile a few hundred
   blocks and pick a median.
2. **Where does cycle accounting LIVE in `psx_check_interrupts`?**
   Before or after `dispatch_count++`? Should it replace it entirely
   in 1.0e-b or coexist for one revision?
3. **Does the dirty_ram_interp need cycle accounting?**
   Probably yes — interpreted blocks should also advance the clock.
   Add a `psx_advance_cycles(N)` call at the end of each interpreted
   block.
4. **Should `psx_advance_cycles` be allowed to fire IRQs?**
   Yes — that's the whole point. But the IRQ-pending bit is set in
   `i_stat`; CPU exception entry only happens at the next call to
   `psx_check_interrupts`, which checks SR/IM/in_exception. No
   recursive entry.
5. **VBlank during exception handler — do we still need a defer?**
   No. Real hardware fires VBlank during handler too; the IRQ pends
   until the handler returns. With proper CPU-side gating, our model
   matches without any defer.

## Hard rules during 1.0e implementation

- No "drop the pad byte" hacks.
- No card-specific paths.
- No multi-slot TX FIFOs without no$psx evidence.
- No `dispatch_count`-based VBlank.
- No `in_exception` gating on hardware time advance.
- No bypassing the chain dispatcher.
- No edits to generated BIOS C.
- No fake directory data.
- All migrations behind `SIO_MODEL_CYCLE_PACED` (already on tree from
  1.0a). Default flips to 1 only when the slice it covers is fully
  validated.

## Vertical slice for first commit (1.0e-a)

Smallest possible. Just the scaffold, no behavior change:

1. New file `runtime/include/psx_cycles.h`:
   ```c
   #ifndef PSX_CYCLES_H
   #define PSX_CYCLES_H
   #include <stdint.h>
   #ifdef __cplusplus
   extern "C" {
   #endif
   extern uint64_t psx_cycle_count;
   void psx_advance_cycles(int cycles);
   uint64_t psx_get_cycle_count(void);
   #ifdef __cplusplus
   }
   #endif
   #endif
   ```

2. New file `runtime/src/psx_cycles.c`:
   ```c
   #include "psx_cycles.h"
   uint64_t psx_cycle_count = 0;

   void psx_advance_cycles(int cycles) {
       if (cycles <= 0) return;
       psx_cycle_count += (uint64_t)cycles;
       /* Phase 1.0e-a: peripheral hooks empty.
        * 1.0e-b will call sio_advance(cycles); etc. */
   }

   uint64_t psx_get_cycle_count(void) {
       return psx_cycle_count;
   }
   ```

3. Add `psx_cycles.c` to `runtime/CMakeLists.txt` for both targets.

4. Expose via TCP in `debug_server.c` as a passive read-only field
   (e.g., add `"psx_cycle_count"` to `freeze_check`).

5. Validation: build clean; baseline behavior unchanged
   (`psx_advance_cycles` not called by anyone yet); `freeze_check`
   shows `psx_cycle_count: 0`.

That's 1.0e-a. After validation passes, 1.0e-b plugs the SIO scheduler
in. Each subsequent step adds one capability and re-validates against
baseline + the previous step.
