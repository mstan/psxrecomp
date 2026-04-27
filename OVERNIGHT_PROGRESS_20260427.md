# Overnight progress — 2026-04-27

## What landed (committed)

### Commit 1 — `fix(memcard): clear FLAG byte after read — the four-day stall`
The FLAG fix in `runtime/src/sio.c` + new `sio_first_divergence` TCP cmd
+ state-12/13 seeds + chain_trace/mc_status from prior session.

**Real bug, single line:** our card sim returned FLAG=0x08 forever instead
of clearing it after first read. BIOS state-3 chain handler sees bit 3
set → "fresh card" path → returns v0=-1 → dispatcher resets counter →
infinite loop. Beetle returns 0x00 → chain advances cleanly.

After fix:
- Both runtimes navigate to MEMORY CARD screen
- Inner read-chain advances states 1..11 cleanly with v0=0
- byte-1 SIO RX divergence eliminated

### Commit 2 — `debug: always-on wtrace coverage for card-chain state addrs`
Per global rule "use ring buffers, never time/attach", added static
always-on wtrace at boot for the addresses that determine card-chain
progress. Lets us answer "when was X written?" deterministically by
querying the ring buffer, no racing live samples.

Always-watched now (`runtime/src/debug_server.c`):
- 0x7514..18 — chain counter
- 0x7528..30 — per-slot chain handler ptr (=0x5688 read / =0x5B64 detect)
- 0x755A    — chain abort flag (state-1/D1 set, outer coord aborts on !=0)
- 0x7520    — read-chain success flag
- 0x74A4    — chain status flag

WTRACE_MAX_RANGES bumped 8→16.

## What's still broken (next downstream divergence)

**Memory cards still don't show save data** — same as the user noted
("we're now at the MEMORY CARD screen but card slots show no save data").

### Deterministic root cause (per ring buffer evidence)

Counter at 0x7514 advances via two distinct chain handlers:
- `func_00005B64` (detection chain, table 0x6CCC, 4 entries D1..D4)
- `func_00005688` (READ chain, table 0x6c98, 13 entries states 1..13)

Read chain DOES get installed at 0x7528[slot] via `func_00005E98` at
seq=12754 (after press X). It runs for ~60 wtrace events then gets
overwritten back to detection by `func_00005FF0` at seq=12814.

During the read-chain window, counter advances **0→1→2→...→11 then RESETS to 0**.
NEVER reaches 12 (state-12 = data-byte handler) or 13 (state-13 = END byte).

The reset writer is `0xBFC14B90: sw $0, 0x7514` inside `func_00005000`
(outer coordinator), reached via this path:
1. Some kernel callback runs `func_000049BC` (which JALs `func_00005000`)
2. `func_00005000` JALs DeliverEvent (`func_00001C5C`)
3. DeliverEvent fires events; chain dispatcher runs 11 times advancing counter 0→11
4. DeliverEvent returns; `func_00005000` continues
5. `func_00005000` reads `[0x755A]` → if !=0, runs abort path (clears 0x755A,
   resets 0x7514 counter)
6. State-1 had set `0x755A=1` at start of chain. State-13's success path
   would normally clear it — but we never reach state-13.

**The deadlock:** chain needs to reach state-13 to clear 0x755A; outer coord
aborts at state-11 because 0x755A still set. Chicken-and-egg.

Counter histogram across ALL 1458 wtrace 0x7514 writes:
```
counter=0:  596  (resets)
counter=1:  198  detection chain runs
counter=2:  198
counter=3:  198
counter=4:  198
counter=5:   10  read chain runs (only 10 attempts!)
counter=6:   10
counter=7:   10
counter=8:   10
counter=9:   10
counter=10:  10
counter=11:  10
counter=12:   0  ← never reached
counter=13:   0  ← never reached
```

### What Beetle does differently (untraced)

Beetle's 0x7528=0x5B64 too (steady state), so it ALSO uses detection
chain primarily. Beetle's `find_first_divergence` at 0x7560 = 0xC3
(checksum from state-12+ data XOR; ours = 0). So Beetle DID reach
state-12+ at some point and complete a directory read.

Hypothesis (NOT yet measured deterministically):
- In Beetle, 13 chain advances complete in ONE DeliverEvent invocation
  (so state-13 clears 0x755A before func_00005000 checks it)
- In ours, only 11 chain advances complete per DeliverEvent invocation,
  abort fires before state-13

WHY only 11? Unknown without further measurement. Possibilities:
- Our SIO IRQ pacing is too slow (each chain advance needs an SIO IRQ;
  if IRQs queue up slower than DeliverEvent processes them, only N fit)
- DeliverEvent's exit condition is different in our recompiled C
- Some kernel function (called during state-1..11 path) terminates
  DeliverEvent early in our build but not on real hw

Confirmed dead ends:
- 0x74F8 FLAG-store divergence (red herring — it's only written by 0x57
  WRITE-cmd path, we never send 0x57)
- 0x7264 slot toggle (benign timing skew)
- All cascade-reset SW sites in dispatcher (ruled out by ra=0x4DDC analysis)

## Next action when you wake up

The principle-aligned move per "build tools, not guesswork":

**Build a B0 call ring.** Trace every BIOS B0:XX function call with
the function index (`$t1`), `$ra`, and seq. Beetle vs ours can be
compared: Beetle's B0:6B40 (DeliverEvent) call frequency, B0:6380
call sequences inside state-11, B0:1B44 calls, etc. The B0 jump table
is at 0x80000060 → 0x000000B0; intercepting that one site gives full
visibility.

The deadlock IS understood. The fix point requires understanding why
DeliverEvent only fires the chain handler 11 times instead of 13. A
B0-call ring buffer is the cleanest way to see that without racing.

Alternative quicker probe: arm wtrace on 0x7514 + 0x755A + the SIO
register area, ONE button press, see EVERY event interleaved by seq —
see if there's an SIO IRQ between counter=11 advance and the reset
that should have advanced to 12 but didn't.

Files modified but not yet committed (from this session):
- `CLAUDE.md` (prior-session edits)
- `recompiler/src/full_function_emitter.cpp` + `.h` (prior session)
- `runtime/src/beetle_psx_bridge.cpp` (prior session)
- `runtime/src/main.cpp` (prior session)
- `runtime/src/memory.c` (prior session)
- `tools/analyze_sio_trace.py` (prior session)

These are all from earlier work and unrelated to today's FLAG bug fix.
Leave them be unless a new session decides they're ready.

## Update — function ENTRY/EXIT trace rings landed (commit 84180e2 + 188bdc8)

Built two new always-on rings hooked off `psx_dispatch`:

- **`fn_entry`** — every dispatch in CALL ORDER with target, $ra,
  $a0..$a3, $t1 (B0/A0/C0 vector index), depth, frame
- **`fn_exit`** — every function in FINISH ORDER with $v0, $v1, plus
  link back to entry seq

Shadow call stack (4096-frame) classifies each dispatch as:
- RETURN — target matches an open frame's saved RA → pop + record exit
- TAIL CALL — $ra unchanged from previous dispatch → replace top frame
- NEW CALL — $ra changed (fresh JAL) → push new frame

TCP commands:
- `fn_filter` `{lo, hi}` — set [lo, hi) physical addr filter
- `fn_clear` — reset both rings + shadow stack
- `fn_stats` — totals, depth, overflows, tail-call count
- `fn_entry_dump` `{count, seq_lo, seq_hi, addr_lo, addr_hi}`
- `fn_exit_dump`  `{count, seq_lo, seq_hi, addr_lo, addr_hi}`

### Validation revealed new info

With filter `0x4000..0x6000` (BIOS card area):

- During press X, captured 4 read-chain runs each going through:
  state-1 → state-2 → ... → state-10 → 0x59D0 → state-11 → 0x5A08 → 0x5A48
- All with `ra=0x4DDC` (chain dispatcher's jalr return)
- The intermediate addresses 0x59D0, 0x5A08, 0x5A48 are functions
  reached via **tail-jump from state addresses** that we didn't see
  before. They live BETWEEN state-10/11 and AFTER state-11 in the
  address space. Worth investigating — they may be where the chain
  decides to abort.

### Known caveat

`fn_exit` records v0 at "control leaves this function" — for fall-
through dispatches this is the moment the next function dispatches,
not the true return-to-caller. So v0 may reflect intermediate state
rather than intended return value. e.g. state-11 exits with v0=0xB
because that's the COUNTER value (chain dispatcher loaded it into v0
before jalr; state-11 didn't overwrite before falling through to
0x5A08).

Real return values still surface for functions that DO set v0 before
returning (e.g., D-states, kernel callbacks) — those are accurate.

### Workflow for the user

```bash
# 1. Set tight filter to scope of interest
{"cmd":"fn_filter","lo":"0x00004000","hi":"0x00006000"}

# 2. Clear before triggering action
{"cmd":"fn_clear"}

# 3. Trigger (press, etc)
{"cmd":"press","buttons":49151,"frames":120}

# 4. Inspect
{"cmd":"fn_stats"}
{"cmd":"fn_entry_dump","count":300,"addr_lo":"0x000056E0","addr_hi":"0x00005AB4"}
{"cmd":"fn_exit_dump","count":300,"addr_lo":"0x000056E0","addr_hi":"0x00005AB4"}

# Use seq_lo/seq_hi to step through specific window
```

## Hard rules to keep

- Never sample-and-hope. Use the always-on rings now extended to cover
  card-chain state. Query, don't time.
- 0x7528 wtrace shows EVERY chain-handler-ptr write since boot. If you
  need to know "when did read chain get armed?", filter that ring.
- The FLAG fix is correct; don't revert. Subsequent reads correctly
  see 0x00 matching real PSX + Beetle.
