# Overlay cross-reference tooling — `tools/overlay_xref.py`

Static analysis of an overlay-heavy PS1 title has a hole in it: **the code you
care about does not exist in the boot EXE.** Titles like Tomba 2 ship a ~165 KB
loader and stream the entire game in as code overlays, so a Ghidra import of
`SCUS_944.54` covers `0x80010000`–`0x80038800` and nothing else. Every
`[widescreen.cull]` site this project has ever needed lives outside that range.

`tools/overlay_xref.py` closes the hole by reading the runtime's overlay capture
set. It is **game-agnostic** — point it at any title's capture directory.

---

## Why the capture set is a legitimate source of truth

The runtime writes one JSON per distinct overlay body it has ever mapped to
`<exe_dir>/overlay_captures.json.d/<contenthash>.json`
(schema `psxrecomp overlay capture v2`):

| field | meaning |
|---|---|
| `load_addr` | virtual base the body was mapped at |
| `size` | body length in bytes |
| `bytes_b64` | the actual bytes that were in guest RAM |
| `executed_pcs` | PCs observed executing inside this body |

The set is **content-hashed and additive across every run**, so it is an
always-on ring buffer that needs no arming — it already contains everything the
game has ever loaded, including bodies from sessions that ended months ago. This
satisfies the project's ring-buffer rule: you *query* it for the window of
interest rather than arming a trace and hoping to catch the event.

For scale, Tomba 2's probe capture dir holds **30 769 captures across 116
distinct load bases**, with ~34 M executed-PC records.

---

## The problem it exists to solve

`bias_sites`, `range_sites`, `screen_x_sites` and `depth_sites` are matched by
**address plus opcode class only**. Overlays genuinely map *different code* to
the same virtual address, so an address-only record can silently rewrite an
unrelated instruction.

This is not theoretical. Measured on Tomba 2's shipped config:

| address | finding |
|---|---|
| `0x80110A08` (committed `bias_site`) | **3 distinct words**: `lw $s1,20($sp)` / `addiu $a1,$v1,0x80` / `addu $v1,$zr,$zr`. The opcode guard saves this one. |
| `0x8013F138` (committed `bias_site`) | **5 variants, all `addiu $v0,$zr,imm`**, immediates `0x155 / 0x124 / 0x208 / 0x320 / 0x224`. Same opcode, so **one address-only record widens all five.** |
| `0x8010E2FC` | **two entirely different overlay bodies** at the same VA (bases `0x8010C000` and `0x8010A000`). |

**Run `word <VA>` on every candidate site before committing it.** If more than
one word appears, use a full expected-word guarded record rather than a bare
address.

---

## Commands

```
overlay_xref.py -d <captures.json.d> <command> [args]
```

| command | purpose |
|---|---|
| `index` | per-base summary: variant count, sizes, how many captures executed |
| `word VA...` | every distinct word at each VA, with capture ids and exec counts |
| `dis VA -n N` | disassemble N instructions, **one listing per distinct variant** |
| `func VA` | walk back to the prologue, forward past `jr $ra`, then list |
| `cover VA...` | which load bases/sizes cover each VA |
| `scan --op OP [--imm-min/--imm-max]` | every site matching an opcode, optionally by immediate range |
| `imm VALUE` | every site whose 16-bit immediate equals VALUE (ALU **and** load/store displacements) |
| `callers VA` | every `jal`/`j` site targeting VA — a call graph without Ghidra analysis |
| `window [--signed] [--gap N]` | find the centered-window gate idiom (below) |
| `ramimage OUT` | composite the captures into a flat guest-RAM image for Ghidra |

A parse of the full Tomba 2 set takes ~85 s cold; results are cached to
`<captures.json.d>/.overlay_xref.cache` (~10 s warm). `--no-cache` forces a
re-parse.

---

## The centered-window gate idiom

Almost every PS1 visibility or proximity gate is written as one unsigned compare
against a recentred delta:

```
subu   rT, rRef, rObj        # signed coordinate delta
addiu  rT, rT, BIAS          # recentre                      <-- bias site
andi   rT, rT, 0xFFFF        # truncate back to 16-bit (optional)
sltiu  rD, rT, SPAN          # |delta| < SPAN/2              <-- range site
```

`window` finds these pairs. Instructions between the bias and the compare are
allowed **only when they carry the value** (a same-register `andi`/`addiu`/
`sll`/`sra`/`or`); a genuine redefinition breaks the pair.

> **The `andi` truncation is part of the idiom, not a clobber.** A naive
> "destination was rewritten, so this isn't a pair" heuristic breaks on it and
> silently reports zero real gates while still finding plenty of false ones.
> That failure is quiet and costs a whole analysis pass.

### Reading the constants

`SPAN == 2*BIAS + 1` is a symmetric window (`|delta| < BIAS`). Anything else is
one-sided or asymmetric and deserves a second look:

| bias / span | shape |
|---|---|
| `0xE6 / 0x1CD` (230 / 461) | symmetric, `|delta| < 230` |
| `0x258 / 0x259` (600 / 601) | one-sided, `delta ∈ [-600, 0]` |
| `0x320 / 0x44D` (800 / 1101) | asymmetric, `delta ∈ [-800, 300]` |

### A trap worth naming

**Most window pairs in a 3D action title are 12-bit ANGLE windows inside
collision tests, not distance gates.** On Tomba 2, 43 pairs were found and the
strongest-looking candidates turned out to be attack/hit cones:

```
sqrt(dx² + dz²)            (jal 0x80084080)
compare against summed radii at actor+0x80
atan2                      (jal 0x80085690)
angle window (andi 0xFFF, bias/span)
sb 1, 43(actor)            # set the hit flag
```

Confirmed for `0x8001FBBC` (bias `0x580` / span `0x301`) and `0x8010E2FC`
(682 / 1365). Widening one of these changes **combat reach**, not what is drawn.
Always read the surrounding function before classifying a pair.

---

## Feeding Ghidra: `ramimage`

Overlay code only exists in RAM at runtime, so the decompiler cannot reach it
from a static EXE import. `ramimage` flattens the capture set into a raw guest
RAM image that Ghidra can analyse:

```bash
python tools/overlay_xref.py -d <captures.json.d> \
    ramimage <game>/ghidra/<title>_ram.bin \
    --manifest <game>/ghidra/<title>_ram_conflicts.txt
```

Import it with `language = MIPS:LE:32:default`, `base_address = 0x80000000`,
compiler `default`, then let auto-analysis run.

Where several overlays mapped different bodies to one address, the winner is the
variant seen **executing** in the most captures (ties broken by capture count).
Every disagreement is written to the conflict manifest.

> ### The image is a composite fiction in the conflicting regions
>
> Tomba 2's image covers **74.4 % of the 2 MB address space** and has
> **44 576 conflicting addresses (~11 % of covered words)**. Inside those
> regions the listing is a blend of overlays that never coexisted.
>
> Treat Ghidra output from this image as a **lead, not a fact**. Confirm every
> instruction you intend to patch with `word <VA>` / `dis <VA>` against the real
> per-overlay variants. See the worked example below for why this is not
> optional.

### Worked example — the decompiler will invent addressing

Ghidra decompiled an actor state machine as reading the player through a global:

```c
uVar2 = FUN_800782b0(param_1 + 0x2c, (int)*(short *)(DAT_800e7f5c + 0x2c), ...);
```

The actual instructions are:

```
0x8006A560: 8E0200DC  lw   $v0,220($s0)     # player via <context>+0xDC
0x8006A568: 8445002C  lh   $a1,44($v0)
```

There is **exactly one** real reference to `0x800E7F5C` in the entire capture set
(`0x80069B70`) — confirmed independently by `imm 0x7F5C` and by Ghidra's own
xrefs. The `DAT_800e7f5c` in that function body is decompiler inference from
constant propagation, not a load. A site list built from the decompiler's view of
addressing here would have been wrong.

Recommended split of labour:

| task | use |
|---|---|
| call graph / xrefs | `overlay_xref.py callers`, `imm`, `scan` — exact, needs no analysis pass |
| readable control flow | Ghidra `create_function` + `get_code decompiler` |
| **anything you will patch** | `word` / `dis` on the real variants — always |

---

## Related

- `docs/config_schema.md` — `[widescreen.cull]` schema
- `docs/WIDESCREEN.md` — native-wide / adaptive view design
- `docs/COMPILING_OVERLAYS.md` — how captures become compiled overlay DLLs
