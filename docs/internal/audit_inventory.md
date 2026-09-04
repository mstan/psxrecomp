# Audit Inventory — psxrecomp-v4

Baseline as of 2026-05-12. Captures what audit / test tooling exists, what
scope it covers, what each tool currently reports, and where the gaps are.

This document is the input for Phase B (codegen-audit expansion) and
Phase C (L2-semantic execute tier).

## Tier structure

```
Layer 5  Runtime behavioral differential vs Beetle (continuous RAM diff)    NONE
Layer 4  Hardware-sim conformance (SIO/DMA/MDEC/GTE vs no$psx)              PARTIAL
Layer 3  Codegen / discovery audits (static analysis of generated C)        PARTIAL
Layer 2b L2-semantic — execute emitted code and verify register state       NONE
Layer 2a L2-structural — emit pattern matching on generated C               HAVE
Layer 1  Per-instruction decode correctness vs Rabbitizer                   HAVE
Layer 0  Build & link integrity                                             HAVE
```

## Tools inventory

### Layer 1 — Decode conformance

**`recompiler/tests/l1_decoder_test.cpp`** (built: `recompiler/build/l1_decoder_test.exe`)
- Scope: BIOS only. Code regions Boot `0x00000-0x0DC60`, Kernel
  `0x10000-0x16760`, Shell `0x18000-0x42800`.
- Compares `MipsDecoder::decode()` mnemonic vs Rabbitizer for every word
  in those regions.
- Buckets mismatches by `oracle_mnem -> our_mnem`.

**Baseline result:** widespread mismatches. Notable buckets:
| Count | Rabbitizer says | We say     | First addr   | Comment |
|------:|-----------------|------------|--------------|---------|
|   181 | `cache`         | `???`      | 0xBFC04300   | **Real gap.** CACHE (op 0x2F) is an R3000A instruction we don't decode. |
|    31 | `mvmva`         | `gte`      | 0xBFC34FF8   | Granularity. We map all GTE cmds to generic `gte`. |
|    13 | `rtpt`          | `gte`      | 0xBFC35DF0   | Same. |
|    13 | `nclip`         | `gte`      | 0xBFC361B4   | Same. |
|    11 | `tge`           | `special?` | 0xBFC10190   | R4000 trap — likely data-in-code or kernel trap encoding. |
|    10 | `dadd`          | `special?` | 0xBFC101D4   | 64-bit (R4000+), likely data. |
|     9 | `rtps`          | `gte`      | 0xBFC35DAC   | GTE granularity. |
|     ~ | `mov.d, mtc1, lwc1, add.d` | `???` | 0xBFC02358 | FP — R3000A has no FPU; likely data-in-code in Boot region. |

Two distinct classes:
- **Real decode gap:** `cache` (op 0x2F). 181 sites. Decoder needs a case.
- **Granularity gap:** GTE sub-mnemonics. Not a correctness bug — our
  emit treats all GTE cmds via `gte_execute`. But audit clarity benefits
  from matching Rabbitizer's specificity.
- **False positives (data-in-code):** dadd, dmult, dsll32, mov.d, mtc1,
  c.eq.d, etc. These are R4000+ / FPU instructions that don't exist on
  R3000A. They appear because we scan the entire region without skipping
  inline data or jump tables. Filter improvement, not a decoder bug.

### Layer 2a — Structural emit conformance

**`recompiler/tests/l2_structural_test.cpp`** (built: `l2_structural_test.exe`)
- For each MIPS instruction form, build word, translate via
  `StrictTranslator`, verify emitted C contains expected patterns
  (e.g., `gpr[2]`, `gpr[4]`, `<< 1u`).
- Snesrecomp-style. Note in source: "does NOT verify semantic
  correctness (that requires L2-semantic with actual execution)."

**Baseline result:** `42/42 ok`.

**Gap:** 42 forms / ~150 R3000A instructions ≈ 28% coverage. No
coverage for: load-delay-slot semantics, mult/div HI-LO timing, LWL/
LWR/SWL/SWR unaligned partial-word, JALR same-source/dest, shift-by-32
edge case, COP0 RFE / SR-bit semantics.

### Layer 2b — Semantic execute conformance

**Status:** does not exist.

The gap. To know "does our emit *do* the right thing when run", we'd:
1. Build a tiny test harness that takes MIPS bytes + initial CPUState,
   runs the recompiler → C → execute, returns final CPUState.
2. For each instruction, supply known input registers and assert output
   matches the R3000A spec.
3. Cross-check against mednafen-PSX's CPU as a reference impl.

This is Phase C.

### Layer 3 — Codegen / discovery audits

**`tools/_codegen_audit.py`** (BIOS only)
- 6 passes against `generated/SCPH1001_full.c` + `_dispatch.c`:
  1. Predicate-init audit (verifies 2026-05-04 fix).
  2. Direct `psx_dispatch(cpu, 0xXXXX)` targets in dispatch table.
  3. Indirect `psx_dispatch(cpu, gpr[N])` distribution.
  4. Tail-call `cpu->pc = ...; return;` targets in dispatch table.
  5. Chained-branch-with-delay-slot-label population.
  6. Highest-frequency missing dispatch targets.

**Baseline result:**
- Predicate decl audit: `buggy=0 zero_init=6104 assigns=6104` ✓
- Direct psx_dispatch: 796 unique, 0 missing ✓
- Indirect sites: 88 (across many registers)
- Tail-call missing: **1 site → `0x00000CF0`** — the install-at-runtime
  SIO data-byte handler stub. Expected, handled by dirty-RAM
  interpreter. Not a regression.
- Chained-branch with delay-slot label: 27 sites, all protected ✓

**`tools/gte_audit.py`** (BIOS only)
- Scans ROM for COP2/GTE instructions, compares against generated code.

**Baseline result:** "missing" hits in `0xBFC75xxx..0xBFC7Fxxx`. These
are in the BIOS DATA region (code ends at `0xBFC42800`). All false
positives from scanning data bytes that happen to decode as COP2 ops.
Need to filter by code region.

**`tools/scan_branch_delay_hazards.py`** (BIOS only)
- For every conditional branch in walked code, checks whether the
  delay slot writes to the branch's rs/rt operand.
- Verifies `strict_translator.cpp` uses the snapshot pattern.

**Baseline result:**
- Branches scanned: 4617
- Delay slot writes branch operand: 441 (intrinsic, informational)
- **Incorrect in current translator: 0 ✓**
- Delay-slot unreachable in walk: 0 ✓

**`tools/check_dispatch.py`** — one-off probe with hardcoded address
list. Not generally useful. **Skip from active inventory.**

### Layer 4 — Hardware-sim conformance

**`runtime/tests/test_sio_card_protocol.c`**
- Drives `sio.c` TX/RX state machine through a 137-byte memcard READ
  command, asserts each RX byte matches no$psx.
- Not run as part of this Phase A pass (built but not invoked here).

**Gap:** other MMIO regions — DMA, MDEC, GPU command stream, timers,
IRQ controller — have no protocol conformance tests.

### Layer 5 — Runtime differential

Does not exist. Both psx-runtime (port 4370) and psx-beetle (port 4380)
speak the same wire protocol, so the infrastructure for paired wtrace
queries is present; what's missing is the diff-and-cluster tool itself.

## Tool generalization status

**All five Layer 1-3 tools are hardcoded to BIOS paths.** They were
written during Phase 1-3 BIOS bring-up and have not been generalized
to game code. For Phase B, generalize them via:
- Argument: `--full-c PATH --dispatch-c PATH --rom PATH` (or read from
  `game.toml`-style config).
- Run each tool once for BIOS, once for Tomba.

## Phase A findings — what to do with them

| Finding | Class | Priority | Phase |
|---------|-------|---------:|-------|
| L1: `cache` (op 0x2F) not decoded — 181 sites | Real decoder gap | High | Phase B |
| L1: GTE sub-mnemonic granularity | Audit clarity | Low | Phase B |
| L1: FPU / 64-bit FPs in data regions | Audit filter | Low | Phase B |
| L2: 42 forms / ~150 instructions = 28% coverage | Test gap | High | Phase C |
| L2-semantic tier does not exist | Architectural | High | Phase C |
| All audits BIOS-only | Tool scope | High | Phase B |
| gte_audit.py false-positives in data region | Audit filter | Low | Phase B |
| Tail-call miss at 0x00000CF0 | Expected stub | None | — |

## Progress log

### Phase B2 (2026-05-12) — First audit pass against Tomba

Ran all three Python audits against Tomba's `SCUS_942.36`. Mixed
outcome — tool gaps surfaced and real bugs surfaced.

**Initial run findings:**
- `codegen_audit.py` returned all zeros — its regexes are BIOS-emit-
  specific and don't match the game `full_function_emitter` shape
  (direct `func_X(cpu)` calls, switch-based dispatch, `_bc_X` branch
  conds). **Required a sibling tool.**
- `gte_audit.py` reported **29 findings** in Tomba text segment
  (8 GTE + 21 LWC2/SWC2). See TombaRecomp ISSUES Issue #2.
- `scan_branch_delay_hazards.py` correctly skipped walker (no
  function_starts file for Tomba) and ran translator-source check.

**`codegen_audit_game.py` created** (per the principle: separate tool
sharing patterns only at the margins). Audits the 6 passes that
matter for game-emit style. After fixing my dispatch-table regex
(Tomba uses `case 0xADDRu: func_X(cpu); return 1;` not BIOS's array
form) and adding code-region filtering for `call_by_address` literals
(331 of 333 are RAM-loaded overlays handled by `dirty_ram_dispatch`,
expected):

| Pass | Result |
|------|--------|
| 1. Direct func_X(cpu) calls | 2,973 calls, 840 unique → **all resolve** ✓ |
| 2. Literal call_by_address  | 333 unique → **2 in-code misses** ⚠ |
| 3. Indirect call_by_address | 360 sites (informational) |
| 4. Tail-call cpu->pc=X      | **0 sites** (game emit doesn't use this idiom) ✓ |
| 5. Branch-cond declarations | 6,708 (informational) |
| 6. goto block_X resolution  | 20,172 sites, **all resolve** ✓ |

**Real bug surfaced: Issue #5 — mid-function split targets not
registered in dispatch table.** Three emit sites in
`code_generator.cpp` (lines 970, 981, 998) emit `call_by_address` for
mid-func continuations without calling `register_cross_function_target`.
This is the same bug class as today's jump-table fix. Tomba has 2
manifestations (`0x800905E4`, `0x80090600` inside `func_800905DC`).
**B2's first pass against Tomba found a real recompiler bug.**

### Phase B0 (2026-05-12) — Audit-config schema

Defined a TOML schema for audit configs. Both BIOS and games consume
the same shape; field names are tolerant (`[program]` or `[game]`,
`rom` or `exe`).

Artifacts:
- `tools/audit_config.py` — shared loader.
- `tools/verify_audit_schema.py` — schema validator.
- `bios/SCPH1001.toml` — BIOS config (3 regions, 2 address remaps).
- `TombaRecomp/game.toml` — extended with `[audit]` block (1 region,
  0 remaps).

### Phase B1 (2026-05-12) — Python audit generalization

Migrated three Python audits to consume the audit-config schema. All
three pass BIOS regression with output matching the Phase A baseline.

| Tool                          | Status   | BIOS regression                                                |
|-------------------------------|----------|----------------------------------------------------------------|
| `tools/codegen_audit.py`      | migrated | identical to Phase A (CLEAN, 0 real findings)                  |
| `tools/gte_audit.py`          | migrated | 73 false-positives eliminated by code-region filter; 7 real findings surfaced |
| `tools/scan_branch_delay_hazards.py` | migrated | identical to Phase A (CLEAN); also runs translator-source check for non-BIOS configs that lack a function_starts file |
| `recompiler/tests/l1_decoder_test.cpp`     | **deferred** | C++ TOML parsing not yet wired; the test still runs against hardcoded BIOS `kRegions[]`. Generalize in a later sub-phase. |
| `recompiler/tests/l2_structural_test.cpp`  | not needed | no per-program input; already generic.                          |

Side effects of generalization:
- `gte_audit.py` surfaced 7 previously-masked findings in Shell code:
  4 missing `gte_execute` emits at `0xBFC34FF8`, `0xBFC3502C`,
  `0xBFC35064`, `0xBFC350C4` (all `0x4A480012` = MVMVA), plus
  3 missing LWC2/SWC2 emits. None of these PCs appear in
  `generated/SCPH1001_full.c` — either a discovery gap (function
  containing them not recompiled) or an emit gap. Investigate in
  Phase B3 or B4.

## Methodology going forward

Every new bug class encountered should follow:
1. Reproduce / characterize the class (not the single instance).
2. **Build the audit FIRST** — find every instance of the class in the
   codebase via tooling, not by manual repro of each.
3. Fix the underlying recompiler / runtime issue.
4. Re-run the audit, confirm class is now empty.
5. Add the audit to the standing audit suite so the class can't
   regress.

The audit suite forms the regression net: "0 findings, audited N sites"
is the only acceptable state for a known class.
