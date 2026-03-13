# MMIO Tracing Guide for PSXRecomp

## Overview

MMIO (Memory-Mapped I/O) tracing logs all reads and writes to hardware register
addresses (0x1F801000–0x1F803FFF) that the game performs. This is invaluable for
diagnosing black screens, missing audio, broken rendering, and other issues where
the game's hardware interactions are silently failing.

On a real PS1, these addresses correspond to GPU, SPU, CDROM, DMA, timer, and
interrupt controller registers. In our recompiler, many of these return 0 or are
silently ignored. Tracing reveals exactly which registers the game expects to
interact with, and in what order.

## How to Enable MMIO Tracing

### Step 1: Add the Tracing Infrastructure

In `runner/src/runtime.c`, add a tracing buffer and control flag near the top:

```c
/* MMIO Tracing — enable with psx_mmio_trace_enable(1) */
static int g_mmio_trace_enabled = 0;
static FILE* g_mmio_trace_file = NULL;

void psx_mmio_trace_enable(int enable) {
    g_mmio_trace_enabled = enable;
    if (enable && !g_mmio_trace_file) {
        g_mmio_trace_file = fopen("mmio_trace.log", "w");
        fprintf(g_mmio_trace_file, "OP,ADDR,VALUE,WIDTH,RA,SP\n");
    }
}

static void mmio_trace(const char* op, uint32_t addr, uint32_t value,
                       int width) {
    if (!g_mmio_trace_enabled || !g_mmio_trace_file) return;
    uint32_t ra = g_diag_cpu ? g_diag_cpu->ra : 0;
    uint32_t sp = g_diag_cpu ? g_diag_cpu->sp : 0;
    fprintf(g_mmio_trace_file, "%s,0x%08X,0x%08X,%d,0x%08X,0x%08X\n",
            op, addr, value, width, ra, sp);
}
```

### Step 2: Instrument Memory Access Functions

Add `mmio_trace()` calls to the MMIO paths in each memory accessor:

**read_word** — when `addr_ptr()` returns NULL:
```c
if (!p) {
    mmio_trace("R", addr, 0, 32);  /* log before returning stub value */
    /* ... existing MMIO handling ... */
}
```

**write_word** — when `addr_ptr()` returns NULL:
```c
if (!p) {  /* MMIO write */
    mmio_trace("W", addr, value, 32);
    /* ... existing MMIO handling ... */
}
```

**read_half / write_half / read_byte / write_byte** — same pattern, with
width=16 or width=8.

### Step 3: Enable at Runtime

In `main_runner.cpp`, call after `psx_runtime_init()`:

```c
psx_mmio_trace_enable(1);
```

Or gate it behind a command-line flag:
```c
if (argc > 2 && strcmp(argv[2], "--mmio-trace") == 0)
    psx_mmio_trace_enable(1);
```

## Reading the Trace Output

The trace file `mmio_trace.log` is CSV with columns:

| Column | Description |
|--------|-------------|
| OP     | `R` = read, `W` = write |
| ADDR   | Full PS1 address (e.g. 0x1F801810) |
| VALUE  | Value read/written (0 for reads of unhandled regs) |
| WIDTH  | Access width: 8, 16, or 32 bits |
| RA     | MIPS return address at time of access |
| SP     | MIPS stack pointer at time of access |

### Key Addresses to Watch

| Address | Register | Notes |
|---------|----------|-------|
| 0x1F801070 | I_STAT | Interrupt status — game polls for VBlank/DMA/etc |
| 0x1F801074 | I_MASK | Interrupt mask — which interrupts are enabled |
| 0x1F801810 | GP0 | GPU data port (commands + pixel data) |
| 0x1F801814 | GP1/GPUSTAT | GPU control port / status register |
| 0x1F801100–0x1F80112F | Timer 0/1/2 | Timer counters, modes, targets |
| 0x1F801040–0x1F80104F | JOY_DATA/STAT/MODE/CTRL | Controller/memory card |
| 0x1F801080–0x1F8010FF | DMA channels 0–6 | MADR, BCR, CHCR per channel |
| 0x1F8010F0 | DPCR | DMA control register (channel enable) |
| 0x1F8010F4 | DICR | DMA interrupt control register |
| 0x1F801800–0x1F801803 | CDROM | CD-ROM status, command, data, interrupt |
| 0x1F801C00–0x1F801DFF | SPU registers | Voice params, control, status |

### Analysis Tips

1. **Unhandled reads returning 0** — Most dangerous. If the game reads a status
   register and gets 0, it may think hardware isn't ready and spin forever, or
   skip initialization. Look for repeated reads to the same address.

2. **Unhandled writes being dropped** — Less immediately dangerous but can cause
   subtle bugs. The game configures hardware that never responds.

3. **Interrupt polling** — The game reads I_STAT (0x1F801070) to check for
   VBlank, DMA completion, etc. If this always returns 0, game loops waiting
   for interrupts will hang.

4. **Timer reads** — Timer counter registers (0x1F801100, 0x1F801110, 0x1F801120)
   should increment. Returning 0 breaks any timing-dependent logic.

5. **DMA completion** — After starting a DMA transfer, games check CHCR bit 24
   (busy flag) or DICR. If we never clear the busy bit, the game hangs.

## Frequency Analysis Script

To find the most-accessed unhandled registers, use:

```bash
# Top 20 most-read MMIO addresses
grep "^R," mmio_trace.log | cut -d',' -f2 | sort | uniq -c | sort -rn | head -20

# Top 20 most-written MMIO addresses
grep "^W," mmio_trace.log | cut -d',' -f2 | sort | uniq -c | sort -rn | head -20

# Find spinning reads (same addr read >100 times from same RA)
awk -F',' '$1=="R" {key=$2","$5; count[key]++} END {for(k in count) if(count[k]>100) print count[k], k}' mmio_trace.log | sort -rn
```

## Known Gaps (as of current implementation)

These MMIO regions are **not handled** and return 0 / drop writes:

- **I_STAT / I_MASK** (0x1F801070/74) — No interrupt emulation
- **Timers** (0x1F801100–0x1F80112F) — No timer counter emulation
- **DMA channels 0,1,3,5,6** — Only channels 2 (GPU) and 4 (SPU) handled
- **DPCR / DICR** (0x1F8010F0/F4) — DMA global control not handled
- **Controller** (0x1F801040–0x1F80104F) — Stubbed in pad read routine only
- **CDROM registers** (0x1F801800–0x1F801803) — Handled via cdrom_stub.cpp

## Integration with Ghidra

When the trace shows a spinning read at a specific RA address, use Ghidra to:
1. Navigate to that RA address
2. Look at the function's decompiled code
3. Identify what MMIO register it's polling and what value it expects
4. Implement the appropriate stub in runtime.c
