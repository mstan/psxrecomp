# Function discovery — `psxrecomp-analyze`

A developer-facing static analysis tool for PS-X EXE images. It exists because
the recompiler already derives everything a modder or decompiler wants —
function boundaries, the call graph, jump tables, argument usage — and then
throws it away after emitting C. This turns that knowledge into an artifact.

It is **not** part of the recompilation path and does not link `runtime/`.

```
recompiler/include/analysis_db.h    schema + the two rules
recompiler/src/analysis_db.cpp      the analysis passes
recompiler/src/analysis_export.cpp  JSON / symbols.toml / Ghidra / TSV writers
recompiler/src/bios_call_names.cpp  A0/B0/C0 kernel call names
recompiler/src/main_analyze.cpp     the CLI
```

---

## The two rules

**1. Static only.** No trace import, no overlay capture set, no executed-PC
feedback, no runtime translation unit in the link. Every function, edge, and
jump table reported was proven from the executable image alone. What static
analysis cannot reach is printed as a *gap*:

```
Static coverage gap (NOT filled from any runtime source):
  132 indirect transfer(s) have no proven target set.
  255504 bytes (40.1%) of the image belong to no code function.
```

The guarantee is enforced structurally: `recompiler/CMakeLists.txt` lists the
tool's sources explicitly and none of them come from `runtime/`.

**2. Tolerant, not fabricating.** CLAUDE.md §0 says an unsupported opcode is a
hard failure — correct for the recompiler, wrong for a tool someone points at a
weird region. The analyzer degrades instead: a region it cannot fully decode is
marked `partial` with the offending PCs listed. That is not a stub. It emits no
code and asserts no semantics it did not prove. Reporting *incomplete* is
allowed here; reporting *fabricated* is not.

---

## Usage

```bash
psxrecomp-analyze <EXE> [options]

# typical: analyse, write the JSON bundle, keep a diffable TSV
psxrecomp-analyze disc/SLUS_005.62 --symbols symbols.toml \
    --out analysis --emit-tsv analysis/functions.tsv

# through the project CLI (finds/builds the binary, reads game.toml)
python3 psxrecomp/psxrecomp_cli.py analyze --project-root . --diff

# or from RetComM Studio → Functions tab
```

Key options: `--exact` (reachability-only partition), `--seeds <file>`,
`--emit-symbols` (merge into `symbols.toml`), `--emit-ghidra`,
`--emit-symbol-addrs`, `--disasm <file> --at <addr>`, `--diff <prev.tsv>`.

### Artifacts

| File | Contents |
|---|---|
| `analysis.json` | image metadata, stats, one record per function |
| `edges.json` | call edges: direct, jump-table, address-taken, resolved indirect |
| `indirect.json` | every indirect transfer, resolved or not, with context |
| `refs.json` | statically resolved memory references (largest; `--no-refs` skips) |
| `functions.tsv` | one line per function — grep/awk friendly, and the `--diff` input |
| `symbol_addrs.txt` | decomp-toolchain symbol map |
| `psxrecomp_import.py` | Ghidra script: creates functions, applies names, bookmarks unresolved sites |

The JSON files are the **stable public interface**. RetComM Studio and any
other front end consume those, never the analyzer's in-process types.

---

## What it recovers

**Function boundaries** come from `FunctionAnalyzer` (the same code the emitters
use), then get scored:

| Confidence | Meaning |
|---|---|
| `verified` | prologue + epilogue + `jr $ra`, fully decoded, called from reachable code |
| `high` | `jr $ra` exit, reachable from the entry point |
| `medium` | prologue-shaped, or address-taken with no proven call path |
| `low` | boundary inferred; unreachable, no prologue, or partial decode |
| `data` | classified as data masquerading as code |

Every row also carries `confidence_reason`, so a claim can be argued with.

**Jump tables.** `resolve_exact_bounded_jump_table()` in `function_analysis.cpp`
is tried first and is authoritative when it fires, but it accepts exactly one
instruction schedule because a wrong table there emits wrong *code*. That is the
wrong trade for a discovery tool: on Masters of Teras Kasi it resolved **0 of
210** indirect sites, because the common schedule puts the `sll` in the branch's
delay slot and materializes the table base between the `sll` and the `addu`.

`recover_jump_table()` walks the actual backward slice instead of matching a
fixed window, and labels how it terminated:

* `jump_table:proven` — the strict resolver accepted it.
* `jump_table:guarded` — an `sltiu` bound on the index register was found, so
  the case count is proven.
* `jump_table:scanned` — no bound found; the table was walked until a word
  stopped looking like a code address. **A hypothesis**, and labelled as one.

**Indirect calls** through a statically-known pointer — materialized inline, or
loaded from a fixed address holding a known function start — are resolved as
`call_ptr:immediate` / `call_ptr:static_slot`. Pointers from struct fields or
computed addresses stay unresolved, correctly.

**Kernel dispatch thunks.** PSY-Q titles call the BIOS through three-instruction
stubs (`addiu $t2,$zero,0xA0; jr $t2; addiu $t1,$zero,<idx>`). The generic rules
score these `low` — no prologue, no `jr $ra` — yet they are among the most-called
functions in any image. They are detected by exact pattern and named from the
A0/B0/C0 tables (`bios_call_names.cpp`, sourced from psx-spx), so
`func_800749DC` with 41 callers reads as `bios_A0_3F_printf` instead.

**Signatures** are inferred by a read-before-write scan: arguments in `$a0–$a3`,
return in `$v0/$v1`, callee-saved set from the prologue's stores, leaf/non-leaf,
GTE/MMIO/syscall use, frame size. This is a **heuristic** — a loop that reads a
register before the backward edge writes it over-reports an argument — so each
record carries `sig.confident`, false whenever the scan hit such a construct.

### Measured on three titles

| Title | Functions | Tables recovered | Indirect left | Coverage |
|---|---|---|---|---|
| Masters of Teras Kasi (`SLUS_005.62`) | 1331 | 46 (804 targets) | 132 | 59.9% |
| Twisted Metal 4 (`SCUS_945.60`) | 1099 | 11 (236 targets) | 185 | 76.1% |
| Klonoa (`SLUS_005.85`) | 230 | 2 (126 targets) | 74 | 71.5% |

Klonoa's boot EXE is 45 KB because the game streams in as overlays — a good
illustration of the limit below.

---

## Where static-only costs coverage

Two classes remain, and the tool says so rather than papering over them:

* **`jalr` through a runtime pointer** — a vtable slot or callback filled in at
  init. No static answer exists in general.
* **Overlay bodies** — for an overlay-heavy title, most of the code is not in
  the boot EXE at all. Closing this statically means parsing the disc
  filesystem, locating overlay blobs as files, and recovering load bases by
  analysing the loader's own `CdRead`/`memcpy` calls. Tractable, not done here.

`tools/overlay_xref.py` mines the runtime's capture set for exactly this, and it
works — but it is dynamic evidence. The rule this tool follows is not "runtime
never touched"; it is **you always know which claims are static**. If capture
import is ever added here, it must arrive as a separately-labelled source, never
silently merged.

---

## Naming loop

`symbols.toml` (see [SYMBOLS.md](SYMBOLS.md)) is the persistence layer. Both
writers are **append/edit-only and never regenerate the file**: it holds more
than functions — Ape Escape has 11 `[[object]]` entries and a top-level `game`
key, Crash Team Racing has 33 `[[site]]` entries, all hand-researched — and an
earlier version that rebuilt the file from parsed `[[func]]` entries silently
deleted every one of them.

```bash
psxrecomp-analyze <EXE> --symbols symbols.toml --emit-symbols \
    --min-confidence verified
```

New functions land as `status = "guessed"` with a note recording why the
analyzer trusted them. A name a human typed always outranks anything inferred,
and an entry this run did not rediscover is carried through rather than dropped.

In Studio, the Functions tab writes the same file one entry at a time via
`project_studio analyze set-symbol`, so a name typed into the table survives the
next analysis run.

---

## Studio integration

RetComM Studio's **Functions** tab is a viewer over the JSON bundle. It never
analyses anything itself: every action shells out to
`python -m project_studio analyze …`, which shells out to `psxrecomp_cli.py
analyze`, which runs this binary. That keeps the CLI the single source of truth
— a headless or CI run produces exactly what the tab shows — and it means Studio
never links the runtime either.

The data path (`src/studio/studio_analysis.cpp`) is deliberately ImGui-free so
it can be tested without a GPU: `./build/analysis_load_test <repo-root>`.

---

## Relationship to the emitters

`psxrecomp-game` answers *"what C do I emit for this?"* and must be conservative,
because a wrong answer is a broken build. `psxrecomp-analyze` answers *"what is
this?"* and can afford a labelled hypothesis, because a wrong answer is a wrong
report that a human will notice. They share the boundary detection and the
strict table resolver; they differ in what they are allowed to guess, and the
labels exist so a consumer can tell which is which.

---

## Widescreen site scanning (`--scan-widescreen`)

Widescreen bring-up on a new title means finding a handful of exact instruction
addresses for `[widescreen.cull]`. Tomba's `game.toml` carries four bias/range
pairs and three `a1` nops, each located by hand from disassembly or by mining
the runtime's capture set with `tools/overlay_xref.py`. Every one of them has a
crisp syntactic signature, so for a main-EXE title that search is static.

```bash
psxrecomp-analyze <EXE> --scan-widescreen --emit-ws-sites ws.toml
python3 psxrecomp/psxrecomp_cli.py analyze --project-root . --scan-widescreen
# or: RetComM Studio → Functions → tick "Widescreen sites" → Analyze
```

### What it finds

**The per-game screen extents.** These are not fixed — Tomba tests `0x140/0x141`
on a 320 display, Ape Escape `0x181` on 368, Wipeout 3 `0x200/0x240` on 512.
Frequency alone picks the wrong values (Wipeout 3's real immediates appear in
only three funnels, drowned by ordinary bounds checks in 83 GTE functions), so
the scan scores **co-occurrence**: the screen-extent signature is a width
compare and a height compare in the *same* function. Pairs are weighted toward
GTE code and toward the console's actual display modes, then completed with the
inclusive-bound sibling when the image contains a compare against it. That last
step is load-bearing — Tomba's pairing keys on `sltiu_imm - 2*bias == W`, and
`449 - 128` is `0x141`, so without it three of its four pairs vanish.

When neither chosen set lands on a real console mode, the report and the emitted
TOML say so rather than presenting a guess with a confident face.

**`auto_screen_x` coverage.** Functions carrying the screen-extent signature are
listed using `ws_cull_detect.h` — the same detector the recompiler and the
runtime interpreter use, so the verdict cannot drift. Knowing that 22 of Tomba's
functions are already handled without any per-address list is the most useful
thing to learn before hand-listing anything.

**bias/range pairs**, the masked-u16 X window:

```
80022E78  addiu $v0, $v0, 64        <- bias_sites   (+halfwidth)
80022E7C  andi  $v0, $v0, 0xFFFF
80022E80  sltiu $v0, $v0, 449       <- range_sites  (W + 2*halfwidth)
80022E84  beq   $v0, $zero, reject
```

The arithmetic tie between the two immediates is what makes this safe to
propose. Without it, any neighbouring `addiu`/`sltiu` would qualify.

**`a1` margin sites** — a repurposable `nop` before a caller-supplied margin
joins an X term. These classifiers are *callees* of the render funnel, not
funnels themselves (Tomba's uses no GTE op and has no static caller at all), so
the signature is local: the function takes `$a1`, folds it in with `addu`, and
has a nop just before that is not already a branch delay slot.

**`screen_x` sites** — width compares in functions `auto_screen_x` cannot reach,
which are exactly the addresses that still need listing by hand.

### Measured against hand-found ground truth

Tomba's `game.toml` sites were found by hand over the course of the widescreen
work. Scanning `SCUS_942.36` with everything auto-discovered:

| key | recall | candidates proposed |
|---|---|---|
| `bias_sites` | **4/4** | 4 (no false positives) |
| `range_sites` | **4/4** | 4 (no false positives) |
| `a1_sites` | **3/3** | 17 |

Independently, the scan recovers Ape Escape's documented `0x181` and both of
Wipeout 3's configured `0x200`/`0x240`.

### Safety

Every proposal already satisfies the instruction form `code_generator.cpp`
requires — `addi`/`addiu` for bias, `sltiu` for range and screen_x, a bare `nop`
for a1 — because the emitter hard-errors on a mismatch in main-EXE mode. A
suggestion that would break the build is not a suggestion. What the scanner
cannot tell you is whether widening a given site *helps*; that is still
playtesting.
