# Axis 4b — SIO1 Serial Link Port Accuracy

Design audit for the new `runtime/src/sio1.c` + `runtime/src/psx_link.c`
(SIO1 register file at `0x1F801050..0x1F80105F` + abstract link endpoint),
written **before** the implementation per repo convention. Normative source is
nocash psx-spx "Serial Port (SIO1)"; secondary references are Beetle/Mednafen
`psx/sio.cpp` (register file only, no link partner) and PCSX-Redux
`src/core/sio1.cc` (semantic reference only — GPLv2, read-not-copy; its
wall-clock socket transport is exactly the design our determinism contract
forbids).

This closes the divergence filed as §2.5 / P6 in
[`axis4_memory_mmio.md`](axis4_memory_mmio.md) ("SIO/serial port region decode
merges two devices"). The counter-example to "near-zero game exposure" is
WipEout 3 Special Edition (SCES-02845), whose statically linked libcomb `"sio"`
device driver programs SIO1 directly (DCB at `0x80179398`, init
`func_801621D8`: CTRL=0x0050 reset strobe → MODE=0x00CE → BAUD=0x00D8 →
CTRL|=0x10 → SysEnqIntRP(3) → CTRL=0x0005, then `I_MASK |= 0x100`).

---

## 1. Register model

Base `0x1F801050`. MODE/CTRL share word `0x1058`; BAUD shares word `0x105C`
(at +2). Lane decode is therefore mandatory and `sio1_read`/`sio1_write` take
the **unaligned** address plus access width — unlike the SIO0 handler, whose
byte paths collapse lanes with `addr & ~3u` (see D8 below).

| addr | reg | access |
|------|-----|--------|
| `0x1050` | TXDATA (w) / RXDATA (r) | read pops the 8-deep RX FIFO |
| `0x1054` | STAT (r) | 32-bit; bits 11..31 = baud timer |
| `0x1058` | MODE (r/w) | 16-bit |
| `0x105A` | CTRL (r/w) | 16-bit |
| `0x105E` | BAUD (r/w) | 16-bit |

### 1.1 STAT

| bit | meaning | model |
|-----|---------|-------|
| 0 | TXRDY1 (buffer not full) | live |
| 1 | RXNE (FIFO non-empty) | live |
| 2 | TXRDY2 / TXDONE (buffer empty + shifter idle) | live |
| 3 | RX parity error | sticky; never set by the deterministic backends (D3) |
| 4 | RX FIFO overrun | sticky; set when a char arrives with FIFO full |
| 5 | RX bad stop bit | sticky; never set by the deterministic backends (D3) |
| 6 | RX input level after stop | always 0 |
| 7 | DSR input (= peer DTR) | live sample of the endpoint |
| 8 | CTS input (= peer RTS) | live sample of the endpoint |
| 9 | IRQ latch | cleared only by CTRL.4 ACK |
| 10 | — | 0 |
| 11-31 | baud timer | computed lazily at read (§2.2) |

Power-on / post-RESET: `STAT & 0x7FF == 0x0005`.

### 1.2 MODE (write mask `0x00FF`)

bits 0-1 reload factor (0,1=MUL1, 2=MUL16, 3=MUL64); 2-3 char length (5..8
data bits); 4 parity enable; 5 parity odd; 6-7 stop bits (0 treated as 1,
1=1, 2=1.5, 3=2).

### 1.3 CTRL (stored mask `0x1FAF`)

bits: 0 TXEN, 1 DTR out, 2 RXEN, 3 TX output level/break (stored, no wire
effect), 4 **ACK strobe** (clears STAT.3/4/5/9, reads back 0), 5 RTS out,
6 **RESET strobe** (reads back 0), 7 stored, 8-9 RX-IRQ FIFO depth
(1/2/4/8), 10 TX IRQ enable, 11 RX IRQ enable, 12 DSR IRQ enable.

Within one write the order is RESET → ACK → store masked value. RESET clears
MODE/CTRL/BAUD, flushes the FIFO, cancels the shifter, clears the IRQ latch,
drives DTR/RTS low and re-anchors the baud timer. The WipEout init sequence
depends on this: it writes `CTRL=0x0050` first and reads CTRL back expecting 0
before OR-ing in the ACK strobe.

### 1.4 Timing

All derived live from MODE/BAUD, recomputed on MODE/BAUD/RESET writes and on
snapshot load, never serialized:

```
bit_cycles    = max((BAUD * factor) & ~1, 1)
char_halfbits = 2*(1 + data_bits + parity) + stop_halfbits   ; 1.5 stop exact
char_cycles   = bit_cycles * char_halfbits / 2
```

WipEout 3 SE: MODE=0x00CE, BAUD=0x00D8 → 3456 cycles/bit (9800 bit/s),
11-bit frame (8-N-2), 38 016 cycles/char.

The STAT baud-timer field is a pure function of
`(now - baud_anchor) mod (bit_cycles/2)` — no per-cycle ticking, rollback-safe
by construction.

### 1.5 IRQ8

One latch (STAT.9), three enable-gated edge sources: RX FIFO count reaching
`1 << CTRL[9:8]`, TX-ready rise (TXRDY1 or TXRDY2 0→1), DSR 0→1. The latch
edge calls `psx_irq_raise(IRQ_SIO1, detail)` (detail 1=RX, 2=TX, 3=DSR); the
CAUSE.IP2 refresh mask `0x7FF` already covers bit 8, so delivery needs no
interrupt-controller change. `sio1_cycles_to_irq` is consulted alongside
`sio_cycles_to_irq` at every event-slicing site so catch-up chunks land on
byte boundaries.

---

## 2. Discrepancies / known simplifications

### D1 — IRQ8 is edge-into-I_STAT, latch-cleared-by-ACK (model)
Same class as axis4 §2.4 (I_STAT edge vs level nuance). We raise once on the
latch's 0→1 and require CTRL.4 before another edge can fire. Real hardware
appears level-ish while STAT.9 holds; no known consumer distinguishes them.
Known-open, mirrors the SIO0 treatment.

### D2 — Wide RXDATA reads pop ONE byte, replicated across lanes (semantic)
psx-spx documents 16/32-bit RXDATA reads as popping multiple FIFO bytes
("preview"-style). Our model pops exactly one and replicates it into the
requested lanes. libcomb reads RXDATA 8-bit; revisit only if a title does
wide pops.

### D3 — Parity / framing errors never occur (model)
The deterministic in-process backends cannot corrupt a character, so STAT.3/5
are sticky-clear in practice. The bits, their ACK-clear path, and the MODE
parity/stop configuration are still modeled so a lossy backend can set them.

### D4 — RXEN=0 discards inbound characters at the wire (semantic)
A char arriving with CTRL.2 clear is drained from the endpoint and dropped
(receiver disabled), not queued for later. Matches UART intuition; psx-spx is
silent. Redux agrees.

### D5 — Empty-FIFO RXDATA read returns the last popped byte (semantic)
Hardware returns stale/undefined bus. We return the last successfully popped
byte (`rx_last`, reset 0) — stable and deterministic.

### D6 — No bit-level shifting (model)
Characters transfer atomically after `char_cycles`; there is no mid-character
observability. Same framing simplification as SIO0's byte-level model
(axis5 D1). STAT.6 (RX input level) is pinned 0 accordingly.

### D7 — Baud timer read is lazily computed, not a counted-down register
Indistinguishable from a ticked timer at every read boundary; documented here
because Beetle counts it down in its event loop.

### D8 — SIO0 byte-lane collapsing NOT fixed here (out of scope)
`memory.c` `mmio_read8_impl`/`mmio_write8` still route SIO0 byte accesses
through `addr & ~3u` (a byte read of `0x104A` hits `0x1048`). SIO1 does its
own lane decode and is unaffected, but the SIO0 bug touches the pad/memcard
path of every shipped title and is filed separately (needs its own env A/B).

---

## 3. Determinism contract (psx_link.h)

The endpoint ops (`tx/rx/rx_peek/set_lines/get_lines`) are pure functions of
endpoint state and the guest cycle argument: no wall-clock, no threads, no
socket I/O. Characters carry a `due_cycle` stamped on the guest timeline at
tx; `rx` releases only entries with `due_cycle <= now`. Snapshots serialize
`due_cycle` and the device's `baud_anchor` as **deltas from the current guest
cycle** (absolute anchors fork after a clock restore — same bug class as
`sio_trace_seq` reseeding in `sio_snapshot_read`). A future socket backend
must ingest on the netplay path at an agreed guest-cycle boundary and stamp
`due_cycle` from the agreed schedule, leaving `rx` a pure ring drain.

---

## 4. Validation method

1. Unit: `sio1_registers_test` (register FSM incl. the exact WipEout init
   sequence and lane decode), `sio1_link_loopback_test` (two devices over the
   crossover; asserts `sio1_cycles_to_irq` is exact at every step),
   `sio1_snapshot_test` (mid-shift round-trip; clock-shifted restore).
   All link only `sio1.c` + `psx_link.c` — the device is an instance with an
   injected clock and IRQ callback, so no runtime stubs are needed.
2. Beetle oracle (`psx-beetle`, port 4380): cable-unplugged register parity
   only — decode, masks, reset. Beetle has no link partner and never raises
   IRQ8.
3. Guest oracle (WipEout 3 SE): CTRL read-back 0 after the 0x0050 reset;
   `sio1_state` reports char_cycles 38016 after init; IRQ8 reaches
   `func_80115800` via the SysEnqIntRP chain and produces
   `DeliverEvent(HwSIO 0xF000000B, EvSpIOE)`.
4. Regression: full suite + per-title selfchecks with `PSX_SIO1_REGS=1`
   (default) and `=0` (legacy fold-into-SIO0 behavior preserved verbatim).
