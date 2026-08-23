# Axis 4 — Memory Map & MMIO Register Semantics: Accuracy Findings

Scope: SEMANTIC correctness (values + side-effects) of the address-space map and
MMIO register dispatch. Cycle-timing accuracy is explicitly OUT of scope (Axis 2).

Sources cross-referenced:
- **Our impl:** `runtime/src/memory.c` (map + dispatch), `dma.c` (DICR), `timers.c`,
  `sio.c`.
- **Oracle (Beetle/mednafen-psx):** `beetle-psx/libretro.cpp` `MemRW<>()` (the memory
  map), `mednafen/psx/irq.cpp` (I_STAT/I_MASK), `mednafen/psx/dma.cpp` (DICR),
  `mednafen/psx/timer.cpp`, `mednafen/psx/cpu.cpp` (`addr_mask[8]`).
- **psx-spx** "Memory Map" / "I/O Map" / "Interrupts" / "Timers".

All file:line citations are against the trees above as of 2026-06-26.

---

## 1. What our implementation does

### 1.1 Address-space translation (`memory.c`)
Every access computes `phys = addr & 0x1FFFFFFF` unconditionally
(`psx_read_word` L695, `psx_read_half` L781, `psx_read_byte` L837, and the write
mirrors), with ONE pre-translation special-case: `0xFFFE0130` (KSEG2 cache/BIU
control) handled before masking (`psx_read_word` L693, `psx_write_word` L734).
This collapses KUSEG (`0x00..`), KSEG0 (`0x80..`), KSEG1 (`0xA0..`) onto the same
29-bit physical space — the intended PS1 mirroring.

### 1.2 Physical region decode (after masking)
- `phys < RAM_SIZE` (0x00000000..0x001FFFFF, **2 MB**) → `ram[]` (L697, L742).
- `0x1F000000..0x1F7FFFFF` → Expansion 1; reads return `0xFFFFFFFF`/`0xFFFF`/`0xFF`
  (open bus), writes ignored (L708, L786, L842, L754).
- `0x1F800000..0x1F8003FF` (**1 KB**) → `scratchpad[]` (L711, L755). Reads/writes
  are NOT gated by IsC; see §2.6.
- `0x1F801000..0x1F803FFF` → MMIO dispatch (`mmio_read*`/`mmio_write*`).
- `0x1FC00000..0x1FC7FFFF` (**512 KB**) → `bios_rom[]` read-only (L721, writes
  silently dropped L773).
- Anything else → `unmapped_fatal()` which **returns 0** and does not abort
  (L364-370, deliberately tolerant for BIOS RAM-size/expansion probing).

### 1.3 MMIO dispatch (`mmio_read32/16/8`, `mmio_write32/16/8`)
Width-specific dispatchers route by address range:
- 0x1F801000..0x1F80103C — memory-control regs, raw `mem_ctrl[16]` array (L377,
  L433); 0x1F801060 — `ram_size_reg` (L385, L443).
- 0x1F801040..0x1F80105F — `sio_read`/`sio_write` (L381, L438).
- 0x1F801070 / 0x1F801074 — I_STAT / I_MASK (`interrupt_write_stat_masked`,
  `interrupt_write_mask_masked`).
- 0x1F801080..0x1F8010FF — DMA (`dma_read`/`dma_write[_masked]`).
- 0x1F801100..0x1F80112F — timers.
- 0x1F801800..0x1F801803 — CDROM; 0x1F801810/14 — GPU; 0x1F801820/24 — MDEC;
  0x1F801C00..0x1F801FFF — SPU; 0x1F802000..0x1F802FFF — EXP2/POST (ignored).
- Unhandled MMIO addr → `mmio_fatal()` which **aborts the process**
  (`psx_fatal_halt`, L343-351).

### 1.4 Interrupt-controller semantics (`memory.c`)
- I_STAT write: `interrupt_write_stat_masked(val, mask)` →
  `i_stat = (i_stat & ~ack_mask) | (i_stat & val & ack_mask)` with
  `ack_mask = mask & 0x7FF` (L266-271). Within the addressed lanes this is
  `i_stat &= val`. **This is the correct write-acknowledge model** (write-0-to-a-bit
  clears it; write-1 keeps it). Subsystems set bits with `i_stat |= (1<<n)`.
- I_MASK write: `i_mask = ((i_mask & ~mask) | (val & mask)) & 0x7FF` (L275).
- I_STAT/I_MASK are kept as **11-bit** values (`& 0x7FF`).
- Byte/halfword stores are handled lane-correctly via shift+mask (L543-551,
  L632-641).

### 1.5 DMA DICR (`dma.c`)
- DICR write (`dma_write_masked` L920-936): `DICR_WRITE_MASK = 0x00FF807F` for
  RW bits; `DICR_RESET_MASK = 0x7F000000` (bits 24-30) handled
  **write-1-to-clear** (`dicr &= ~(val & reset_mask)`). Bit 31 is computed on read
  (`dicr_master_flag`, L182). I_STAT bit3 latched only on the master-flag 0→1 edge
  (`raise_dma_irq_on_master_edge`, L191-198). **This matches hardware/Beetle.**

### 1.6 Guest-read cycle wrappers
`psx_guest_read_*` add +6 wait cycles for main-RAM targets (Axis 2 territory,
noted for completeness, L856-879).

---

## 2. Discrepancies vs Beetle + psx-spx

### 2.1 RAM mirror window is 2 MB, hardware decodes 8 MB (4× mirror) — REAL DIVERGENCE
- **Beetle:** `libretro.cpp` MemRW L789 decodes `if (A < 0x00800000)` (an **8 MB**
  window) then indexes `MainRAM->Read<T>(A & 0x1FFFFF)` (L814) — the 2 MB DRAM
  **mirrors 4 times** across 0x000000..0x7FFFFF in each KUSEG/KSEG segment. (Retail
  consoles have 2 MB; the upper 6 MB are aliases of the same chips.)
- **Ours:** `memory.c` L697/742 gate on `phys < RAM_SIZE` (`RAM_SIZE = 2 MB`,
  L25). Physical 0x00200000..0x007FFFFF falls through every region test to
  `unmapped_fatal()` → **returns 0 on read, silently drops writes** (L728/777).
- **Impact:** A guest (or relocated kernel code) touching a RAM mirror at
  0x00200000+ / 0x80200000+ reads 0 instead of the aliased DRAM byte, and its
  write is lost. Most code stays within 2 MB, but mirror access is legal and the
  BIOS RAM-size routine writes a sentinel at an aliased offset to probe installed
  RAM. Divergence is silent (no fatal), so it can corrupt state without a crash.

### 2.2 Address masking is unconditional `& 0x1FFFFFFF`; hardware masks per-segment — DIVERGENCE (mostly benign, one real edge)
- **Beetle:** `cpu.cpp` L69-76 `addr_mask[8]` indexed by `A >> 29`:
  KUSEG idx0-3 = `0xFFFFFFFF` (no strip), KSEG0 idx4 = `0x7FFFFFFF`,
  KSEG1 idx5 = `0x1FFFFFFF`, KSEG2 idx6-7 = `0xFFFFFFFF`. The masked address is
  then region-decoded; KSEG2 `0xFFFE0130` survives unmasked to reach the BIU.
- **Ours:** unconditional `addr & 0x1FFFFFFF` for all segments except the single
  `0xFFFE0130` special-case.
- **Impact:**
  - KSEG0/KSEG1/KUSEG-low → equivalent in practice (after stripping, both reach
    the same 29-bit phys).
  - **KSEG2 (0xC0000000..0xFFFFFFFF):** hardware does NOT strip the high bits;
    only `0xFFFE0130` is a real register, everything else is "unknown
    write/read=0" in Beetle (L1093-1101). Ours masks 0xC0000000 down into the
    0x00.. RAM/MMIO space and may hit RAM or a device. Any KSEG2 access other than
    the BIU is mis-decoded. (Low practical exposure — games rarely touch KSEG2 —
    but it is incorrect.)
  - KUSEG addresses ≥ 0x20000000 on hardware would be left unmasked (and become
    "unknown"); ours masks them into a mirror.

### 2.3 I_STAT / I_MASK upper bits read back differently — DIVERGENCE
- **Beetle:** `irq.cpp` IRQ_Read L101-114 returns `Status` (or `Mask`) **OR'd with
  `0x1F800000`** before the lane shift — i.e. the unused upper bits of the 32-bit
  register read back as **open-bus/garbage `0x1F80xxxx`**, not 0. `Status`/`Mask`
  are stored as `uint16_t` (L22-24), so bits 11-15 are also live storage.
- **Ours:** I_STAT/I_MASK masked to `0x7FF` on both write and read; bits 11+ read
  back as **0**, and bits 11-15 cannot be stored at all.
- **Impact:** A guest reading I_STAT/I_MASK as a full word and comparing/masking
  the high bits sees `0x00000xxx` from us vs `0x1F80_0xxx` from hardware. Real PS1
  has 11 IRQ sources (bits 0-10), so the *functional* IRQ bits are correct, but
  any code that ANDs the raw register against a wide mask diverges. Low-risk but a
  faithfulness gap; trivially fixable by OR-ing the open-bus constant on read.

### 2.4 I_STAT write-ack uses `&val` but ignores Beetle's level-reassert nuance — MINOR
- Our ack (`i_stat &= val` within lanes) is correct write-0-clears semantics and
  matches `irq.cpp` IRQ_Write L93 (`Status &= V`). **No discrepancy in the ack
  itself.** The architectural difference is in *re-latching*: Beetle drives IRQ
  lines through `IRQ_Assert(which, level)` (irq.cpp L57-79) with edge detection
  `Status |= (old_Asserted ^ Asserted) & Asserted`, i.e. a **level-triggered**
  source that re-latches Status on a 0→1 line transition even after the guest
  acks. Our subsystems set `i_stat |= bit` only at discrete fire points
  (timers `timer_fire_irq`, DMA master-edge), so a device that *holds* its line
  high (e.g. SPU IRQ, CDROM) will not re-raise I_STAT after the guest clears it
  the way hardware does. Semantics correct for edge-style sources (timers/DMA),
  potentially wrong for level-held sources. Flagged for IRQ axis cross-ref.

### 2.5 SIO/serial port region decode merges two devices — DIVERGENCE (low impact)
- **Beetle:** splits `0x1F801040..0x1F80104F` → **FIO/JOY** (controller+memcard,
  `PSX_FIO`, L958) from `0x1F801050..0x1F80105F` → **SIO** (the unused serial
  port, `SIO_Read/Write`, L970). Two distinct register files.
- **Ours:** routes the entire `0x1F801040..0x1F80105F` to `sio_read`/`sio_write`
  (memory.c L381/438), which only decodes 0x1040..0x104E and `return 0` /
  no-op for the default (sio.c L1356, L1317). So 0x1050..0x105F reads return 0 and
  writes are dropped, whereas Beetle maintains real SIO_STAT/MODE/CTRL/BAUD there.
- **Impact:** The serial SIO at 0x1050 is essentially unused by retail games, so
  near-zero practical exposure — but it is a structural inaccuracy (one handler for
  two devices) and 0x1050 reads return 0 vs Beetle's modeled values.
- **RESOLVED:** `0x1F801050..0x105F` now routes to its own SIO1 device
  (`runtime/src/sio1.c` + `sio1_runtime.c`, all six dispatchers in memory.c,
  IRQ8 via `psx_irq_raise`, `BS_SEC_SIO1` snapshot). Full audit:
  [`axis4_sio1_serial.md`](axis4_sio1_serial.md). Legacy decode retained
  behind `PSX_SIO1_REGS=0`. The "near-zero exposure" premise had a
  counter-example: WipEout 3 SE's libcomb link-cable driver.

### 2.6 Scratchpad write under IsC (Isolate Cache) is dropped — likely WRONG
- **Ours:** `psx_write_*` early-out `if (sr_ptr && (*sr_ptr & 0x10000u)) return;`
  (L738, L803, L882) drops **all** RAM *and scratchpad* writes while COP0 SR.IsC is
  set. The scratchpad (0x1F800000) **is** the D-cache data array; on hardware
  cache-isolated writes are exactly how the scratchpad/cache is manipulated, and
  scratchpad reads/writes should still land. By dropping scratchpad writes during
  IsC we can lose legitimate scratchpad stores done while the cache is isolated.
- **Beetle:** the IsC/cache-isolation handling lives in cpu.cpp's load/store path
  (the D-cache-as-scratchpad model); scratchpad at 0x1F800000 is normal addressable
  memory (`MDFNMP_AddRAM(1024, 0x1F800000, ScratchRAM.data8)`, libretro.cpp L2133)
  and not gated off by IsC in the MemRW data path.
- **Impact:** Edge case (most IsC use is the BIOS cache-flush dance writing to RAM
  addresses, which *should* be dropped), but dropping 0x1F800000-range writes under
  IsC is over-broad. Needs verification against the exact BIOS cache routine before
  changing. Flagged as suspect, not confirmed-wrong.

### 2.7 `unmapped_fatal` read returns 0; hardware/Beetle return open-bus — MINOR
- **Ours:** truly-unmapped reads return 0 (L364-370, L729). Expansion-1 correctly
  returns `0xFFFFFFFF` (L709), matching Beetle's `V = ~0U` (L1056).
- **Beetle:** unknown reads set `V = 0` too (L1099) — so for the *unknown* class we
  agree. The divergence is only the RAM-mirror hole (§2.1) reaching this path.

### 2.8 EXP2/POST and memory-control registers are write-through with no masking — MINOR
- **Ours:** `mem_ctrl[16]` stores raw values (L434); EXP2 0x1F802000..2FFF returns
  0 / ignores (L413, L484).
- **Beetle:** memory-control (`SysControl`) applies per-register **write masks**
  `SysControl_Mask[9]` and **read OR** `SysControl_OR[9]` (libretro.cpp L561-567,
  L948/952) — e.g. EXP1 base reads back with `0x1F000000` forced, writes masked to
  `0x00FFFFFF`. Ours returns the raw last-written value with no mask/OR.
- **Impact:** A guest that reads back a memory-control register and checks the
  fixed high byte (BIOS does light-touch validation here) sees our raw value vs
  Beetle's masked/OR'd value. Low practical impact; the BIOS mostly writes known
  constants. Faithfulness gap.

### 2.9 Timer mode-read clears flags (correct) but read-count has a nondeterministic hack — MIXED
- Mode read clearing bits 11/12 (`MODE_TARGET_FLAG | MODE_OVERFLOW_FLAG`,
  timers.c L296-298) **matches** psx-spx ("Cleared on mode/status read") and Beetle.
- **But** count read (`case 0x00`) under `!PSX_ENABLE_BLOCK_CYCLES` does
  `counter += 8` (or +1) as a "simulate continuous counting" hack (L280-291) — this
  injects a synthetic value not derived from the guest cycle counter, so the count
  read is **nondeterministic vs Beetle**, which derives the counter from
  `TIMER_Update(timestamp)` (timer.cpp). This is the Axis-2 cycle issue surfacing in
  a semantic read; flagged because it changes the *value* a guest observes, not just
  timing. (In `PSX_ENABLE_BLOCK_CYCLES` builds this hack is compiled out.)
- Timer `mode` storage masks writes to `& 0x03FF` (L318) and force-sets
  `MODE_IRQ_REQUEST` (bit10). psx-spx: bit10 (IRQ request, inverted) reads 1 when no
  IRQ pending — consistent. Acceptable.

### 2.10 24-bit (`lwl`/`lwr` partial) access not modeled in MMIO path — INFORMATIONAL
- Beetle has explicit `Access24` paths (`PSX_MemRead24`/`Write24`, libretro.cpp
  L1114/L1142) for RAM and ROM. Ours has only 8/16/32 entry points
  (`psx_read_word/half/byte`); 24-bit unaligned accesses are synthesized by the
  recompiler as byte/half combos against RAM, so the MMIO path never sees a 24-bit
  access. Not a divergence for MMIO (no real device is accessed 24-bit), noted for
  completeness.

---

## 3. Prioritized fix list

**P1 — RAM mirror (§2.1).** Change the RAM decode from `phys < RAM_SIZE` to the
hardware 8 MB window with 2 MB wrap: gate on `phys < 0x00800000` and index
`ram[phys & 0x1FFFFF]` in all six `psx_read_*`/`psx_write_*` paths. This is the
highest-impact correctness fix: it is silent today (returns 0 / drops writes) so it
can corrupt state with no crash, and it is the only path that currently routes a
legal RAM access to `unmapped_fatal`. Faithful, class-level, benefits every title.

**P2 — IsC scratchpad gating (§2.6).** Confirm against the BIOS cache-flush routine
(docs/psx_bios_disasm.txt + Ghidra), then narrow the IsC write-drop to RAM
(`phys < 0x00800000`) and STOP dropping scratchpad (0x1F800000) writes. Verify no
boot regression via the oracle before/after.

**P3 — Level-triggered IRQ re-latch (§2.4).** Cross-reference with the IRQ axis:
route SPU/CDROM (line-held) IRQ sources through an `IRQ_Assert(which, level)`-style
edge detector so they re-raise I_STAT correctly after a guest ack, matching
irq.cpp L57-79. Do NOT regress the edge-style timers/DMA path.

**P4 — KSEG2 / per-segment masking (§2.2).** Replace the blanket `& 0x1FFFFFFF`
with Beetle's `addr_mask[A >> 29]` table so KSEG2 (except 0xFFFE0130) is treated as
unknown rather than mis-decoded into RAM/MMIO. Low practical exposure but removes a
whole class of silent mis-decodes.

**P5 — I_STAT/I_MASK open-bus high bits (§2.3).** On read, OR the unused upper bits
with the `0x1F800000` open-bus constant (and widen storage to 16 bits if any title
is found relying on bits 11-15). Faithfulness; trivial.

**P6 — Memory-control mask/OR (§2.8) + SIO/serial split (§2.5).** Apply
`SysControl_Mask`/`SysControl_OR` to the memory-control registers, and split the
0x1F801050..105F serial-SIO into its own (even if minimal) register file rather than
folding it into the JOY handler. Lowest priority — near-zero game exposure.

**Out of scope (Axis 2, noted):** timer count-read `+8` hack (§2.9) and the
RAM-read wait-cycle wrappers (§1.6) are cycle-axis items; they alter observed values
only because counters are not yet derived on-demand from the global guest-cycle
counter. Fold into the FAITHFUL_TIMING_PLAN timer-on-demand work, not here.

---

## 4. Validation method (ring-buffer-first, two-process)

Per CLAUDE.md §16: query the always-on rings on both ports, never arm-and-capture.
psx-runtime = port 4370 (BIOS) / 4500 (build-t2 Tomba2); psx-beetle = port 4380
(per the Tomba2 memory, beetle dev port 4382). Adjust ports to the live binaries.

1. **MMIO read/write trace diff.** The MMIO trace ring already exists
   (`debug_server_trace_mmio_read`/`_write`, wired through `mmio_read*`/`mmio_write*`
   in memory.c). Add the matching always-on ring on the Beetle side (hook
   `PSX_MemRead*/PSX_MemWrite*` in libretro.cpp, or `MemRW` directly) keyed by
   address + width + value. Free-run both to the same scripted input, then query
   `[start_idx, end_idx]` on each and diff the (addr, width, value) tuples. Focus
   ranges: 0x1F801070/74 (I_STAT/I_MASK), 0x1F8010F4 (DICR), 0x1F801100-112F
   (timers). A value mismatch at the same logical access = a semantic bug.

2. **RAM-mirror diff probe.** Specifically targets §2.1: write a sentinel to
   0x00200000 (a mirror offset) via the guest on both cores, then read 0x00000000
   on both. Hardware/Beetle: the sentinel appears at 0x00000000 (aliased); ours:
   0x00000000 unchanged and the 0x00200000 write was dropped. This is the
   pass/fail oracle for the P1 fix. Use the existing `read_ram`/`emu_read_ram`
   commands; no new tooling needed beyond the mirror-offset read.

3. **I_STAT/I_MASK readback diff.** Read 0x1F801070 / 0x1F801074 as a full word on
   both cores at a quiescent point; compare. Expected pre-fix divergence: ours
   `0x0000_0xxx` vs Beetle `0x1F80_0xxx` (validates §2.3 / P5).

4. **DICR ack pinning test.** Drive a DMA completion, read DICR (expect bit31 +
   channel flag set), write-1-to-clear the channel flag, re-read. Compare the
   computed bit31 transition and the I_STAT bit3 latch behavior against Beetle's
   `DMAIntControl`/`DMAIntStatus`/`RecalcIRQOut` (dma.cpp L86-95, L577-583). This is
   a full pinning test, not a one-shot — capture from the rings on both sides.

5. **Regression gate.** Run the diff against BIOS boot + Tomba/Tomba2 title +
   gameplay on both cores after each P-fix; the MMIO trace diff must not introduce
   NEW divergences. Per global rule: regen all banks / run all tests / build all
   configs when the memory.c change lands (it is a runtime-only change — no regen —
   but rebuild psx-runtime AND psx-beetle and re-run the full diff suite).

---

### Summary of correctness verdicts
- I_STAT write-ack (write-0-clears): **CORRECT** (matches `Status &= V`).
- I_MASK: **CORRECT** (functional 11 bits); high-bit readback **WRONG** (reads 0
  not open-bus `0x1F80xxxx`).
- DICR write-1-to-clear + computed bit31 + master-edge I_STAT latch: **CORRECT**.
- Timer mode-read flag clear: **CORRECT**; count-read `+8`: cycle-axis nondeterminism.
- Byte/halfword lane handling for 32-bit regs: **CORRECT**.
- RAM mirroring (2 MB only, no 4× alias): **WRONG (P1)**.
- Per-segment address masking (KSEG2): **WRONG but low-exposure (P4)**.
- IsC dropping scratchpad writes: **SUSPECT (P2)**.
