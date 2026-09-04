# Relocation Manifest Format

> **STATUS AMENDMENT (2026-07, BIOS profiles).** The bulk boot-time
> ROM-to-RAM copy case this document originally targeted is now carried by
> the BIOS profile instead: `[[recompiler.address_model.copy]]` in
> `bios/<STEM>.toml` (single source of truth for the recompiler, the
> emitted C, and the runtime via `psx_bios_image`; see
> `recompiler/src/bios_address_model.h`). The kernel-bless mechanism in
> `memory.c` remains the runtime enforcement of the "verify live RAM
> against the ROM source before dispatching AOT code" contract described
> here. This document stays authoritative ONLY for the oracle-observed
> case — regions with `src_rom_addr: null` that cannot be seen statically
> — which remains unimplemented.


**Status:** Phase B (design + first iteration), 2026-04-30.
**File on disk:** `generated/address_aliases.json`.
**Authority:** `PLAN.md:941-967` ("BIOS Relocation Is a Hard Gate"), `CLAUDE.md:333-365` (Rule 18).
**Companion files:** `generated/normalization_rule.md`, `generated/relocation_proofs/<name>/`.

---

## Purpose

Records every region of code that lives at a RAM address at runtime but
that the static recompiler cannot see in `SCPH1001.BIN` byte-for-byte
at compile time. Two flavors:

1. **ROM↔RAM aliases.** BIOS copies bytes from `0xBFCxxxxx` to a kernel-RAM
   address during init and runs the RAM copy. Same bytes, two addresses.
2. **Trace-discovered RAM regions.** BIOS installs code at a RAM address
   via a mechanism we may not have statically identified (relocated copy,
   patched copy, runtime-assembled, etc.). We have observed bytes via the
   oracle.

Both flavors share one record shape so the recompiler can ingest them
uniformly. The `discovered_by` field records *how* the entry was proven.

---

## File shape

`generated/address_aliases.json`:

```json
{
  "version": 1,
  "normalization_rule": "addr & 0x1FFFFFFF",
  "entries": [ /* AliasEntry[] */ ]
}
```

`version` increments on incompatible schema changes. `normalization_rule`
mirrors `generated/normalization_rule.md` so the recompiler can check
agreement at load time.

### AliasEntry

| Field | Type | Required | Notes |
|---|---|---|---|
| `name` | string | yes | Slug, used as proof-directory name and as part of emitted function names. `[a-z0-9_]+`. |
| `dst_ram_addr` | hex string | yes | Physical RAM address (post-normalization). E.g. `"0x00005800"`. Recompiled function dispatch keys are derived from this. |
| `length` | int | yes | Byte length of the region. Must be a multiple of 4 (MIPS instruction size). |
| `entrypoints` | array of hex string | yes | RAM addresses at which `psx_dispatch` may enter this region. The recompiler emits a separate compiled function per entrypoint. At least one entry, must include `dst_ram_addr` if the region's first instruction is callable. |
| `bytes_hash` | string | yes | `sha256:<64 hex chars>` of the region bytes. Runtime verifies against live RAM before dispatching the AOT function. |
| `bytes_file` | string | yes | Path (relative to repo root) to the raw bytes file under `generated/relocation_proofs/<name>/bytes.bin`. Recompiler reads from here when `src_rom_addr` is null. |
| `src_rom_addr` | hex string | optional | Physical ROM address the bytes were copied from, if known. E.g. `"0x1FC2D800"`. When present, recompiler reads from ROM and additionally checks ROM[src..src+length] hash matches `bytes_hash`. |
| `discovered_by` | object | yes | Provenance. See below. |

### `discovered_by`

| Field | Type | Required | Notes |
|---|---|---|---|
| `tool` | string | yes | Identifier of the oracle / static tool. E.g. `"beetle_psx_oracle"`, `"ghidra_static_copy_loop"`, `"recomp_install_hook_log"`. |
| `writer_pc` | hex string \| null | when known | PC of the instruction that wrote the LAST byte of this region during install. `null` if not observed. |
| `first_exec_pc` | hex string \| null | when known | First PC inside this region that the oracle executed. Useful for cross-validating `entrypoints[0]`. |
| `reason` | string | yes | One-line human explanation of why this region matters and what evidence drove its inclusion. |
| `captured_at` | ISO-8601 date | yes | When the proof was captured. |
| `captured_session` | string | optional | Free-form session/commit reference. |

---

## Proof directory

Per PLAN.md every entry must have a directory `generated/relocation_proofs/<name>/`
containing:

| File | Required | Purpose |
|---|---|---|
| `proof.json` | yes | Canonical record of the entry (full AliasEntry plus extra evidence fields). |
| `bytes.bin` | yes | Raw bytes of the region, exactly `length` bytes. |
| `wtrace_excerpt.json` | when discovered_by.tool=oracle | Slice of the Beetle wtrace ring covering the writer events. |
| `dispatch_excerpt.json` | optional | Slice of recomp dispatch_tail showing the gap (we never dispatched these PCs). |
| `ghidra_*.{json,png}` | when discovered_by.tool starts with ghidra | Static analysis artifact. |

`address_aliases.json` is GENERATED from the proof directories — never
hand-edited. A small tool (`tools/build_address_aliases.py`, Phase C)
walks `generated/relocation_proofs/`, validates each `proof.json` +
`bytes.bin`, and emits the top-level manifest.

---

## Normalization

Working rule (per PLAN.md Phase 1e draft): `normalized = addr & 0x1FFFFFFF`.

Handles the four PSX address-space windows:

| Region | Virtual base | Normalized |
|---|---|---|
| KUSEG (raw) | 0x00000000 | self |
| KSEG0 (cached) | 0x80000000 | strip top 3 bits |
| KSEG1 (uncached) | 0xA0000000 | strip top 3 bits |
| BIOS via KSEG1 | 0xBFC00000 | 0x1FC00000 (distinct from RAM) |

So `0x00007568`, `0x80007568`, `0xA0007568` all normalize to `0x00007568`
and refer to the same RAM byte. `0xBFC07568` stays at `0x1FC07568` — a
distinct ROM byte. The recompiler stores all dispatch keys in normalized
form.

---

## First-iteration scope

Today's first manifest entries cover only the two dark RAM windows
identified in `docs/internal/CASE_A_AOT_GAP.md`:

| name | dst_ram_addr | length | entrypoints |
|---|---|---|---|
| `memcard_handler_4f00` | 0x00004F00 | 0x100 | 0x4F0C, 0x4F54 |
| `memcard_handler_5800` | 0x00005800 | 0x800 | 0x5800, 0x5DB8, 0x5DD0, 0x5DD8, 0x5EF4, 0x5FA8 |

Both are flagged `discovered_by.tool="beetle_psx_oracle"`, both have
non-null `writer_pc` (from the wtrace) and `first_exec_pc`. Both have
`src_rom_addr=null` in this iteration — the BIOS source these were
copied from is not yet identified statically. When we identify the
source (e.g. via tracing the BIOS init copy loops in Beetle and
matching them against ROM), we'll set `src_rom_addr` and drop
`bytes.bin` from the proof directory.

Future iterations expand coverage and add static src identification.

---

## Failure modes the manifest is designed to catch

- **Stale AOT dispatch.** If live RAM bytes drift from `bytes_hash`,
  runtime aborts loudly rather than running the wrong code.
- **Duplicate logical function.** If two entries normalize to the
  same `dst_ram_addr`, the recompiler refuses to build.
- **Unproven alias.** If an entry exists in `address_aliases.json` but
  the matching `relocation_proofs/<name>/` directory is missing or
  has the wrong hash, the recompiler refuses to build.
- **Speculative addition.** Every entry must point to a proof. No
  "I think this region exists" entries.
