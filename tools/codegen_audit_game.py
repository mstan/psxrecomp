"""codegen_audit_game — audit generated C from the game full-function-emitter.

Sibling of `codegen_audit.py` (which audits BIOS strict-translator emit).
The two emit conventions differ enough that the audit passes themselves
diverge — see `docs/internal/audit_inventory.md` for the rationale.

Tomba / game-emit characteristics:
  - Direct function calls: `func_XXXXXXXX(cpu);`
  - Indirect dispatch:    `call_by_address(cpu, 0xXXXX);` or `call_by_address(cpu, cpu->gpr[N]);`
  - Tail call (same as BIOS): `cpu->pc = 0xXXXX; return;`
  - Branch cond:          `int _bc_XXXXXXXX = (expr);` (declare-and-assign in one line)
  - Block label:          `block_XXXXXXXX:`
  - Function def:          `void func_XXXXXXXX(CPUState* cpu) { ... }`

Audit passes:
  1. Direct-call resolution: every `func_XXXXXXXX(cpu)` call references a
     defined function (forward-declared in dispatch_c, defined in full_c).
  2. Literal `call_by_address(cpu, 0xXXXX)` target resolves to dispatch
     table.
  3. Indirect `call_by_address(cpu, cpu->gpr[N])` site distribution.
  4. Tail-call (`cpu->pc=X; return;`) target resolves to dispatch table.
  5. Branch-condition count & distribution per function.
  6. `goto block_X` target resolves to a labelled block in the same
     function (informational; cross-function gotos surface here).

Usage:
  python tools/codegen_audit_game.py --config /path/to/game.toml
"""
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import audit_config


# ── Patterns shared with codegen_audit.py (BIOS emit) ──────────────────
RE_TAIL_CALL = re.compile(r'cpu->pc = 0x([0-9A-Fa-f]+)u?;\s*return;')

# Game emit uses a switch/case dispatch table:
#   case 0xADDRu: func_ADDR(cpu); return 1;
# This is NOT the BIOS strict-translator's `{ 0xADDR, func_ADDR }` array.
RE_DISPATCH_TABLE_ENTRY = re.compile(
    r'case\s+0x([0-9A-Fa-f]+)u?\s*:\s*func_[0-9A-Fa-f]+\s*\(cpu\)'
)

# ── Patterns specific to game full-function-emitter ────────────────────
RE_DIRECT_CALL = re.compile(r'\bfunc_([0-9A-Fa-f]+)\(cpu\);')
RE_CALL_BY_ADDR_LIT = re.compile(
    r'call_by_address\(cpu,\s*0x([0-9A-Fa-f]+)u?\)\s*;'
)
RE_CALL_BY_ADDR_REG = re.compile(
    r'call_by_address\(cpu,\s*cpu->gpr\[(\d+)\]\)\s*;'
)
RE_BC_DECL = re.compile(r'\bint\s+_bc_([0-9A-Fa-f]+)\s*=\s*\(')
RE_FUNC_DEF = re.compile(r'^void\s+func_([0-9A-Fa-f]+)\s*\(CPUState\s*\*\s*cpu\s*\)\s*$', re.M)
RE_FUNC_FWD = re.compile(r'^extern\s+void\s+func_([0-9A-Fa-f]+)\s*\(', re.M)
RE_BLOCK_LABEL = re.compile(r'^(block_[0-9A-Fa-f]+):', re.M)
RE_GOTO_BLOCK = re.compile(r'\bgoto\s+(block_[0-9A-Fa-f]+)\s*;')


def main():
    cfg = audit_config.from_argv(sys.argv)
    print(f"# codegen audit (game emit): {cfg.name}")
    print(f"  full_c     = {cfg.full_c}")
    print(f"  dispatch_c = {cfg.dispatch_c}")
    print()

    if not cfg.full_c.exists():
        print(f"ERROR: full_c not found: {cfg.full_c}", file=sys.stderr)
        return 2
    if not cfg.dispatch_c.exists():
        print(f"ERROR: dispatch_c not found: {cfg.dispatch_c}", file=sys.stderr)
        return 2

    full_c = cfg.full_c.read_text(encoding="utf-8", errors="replace")
    dispatch_c = cfg.dispatch_c.read_text(encoding="utf-8", errors="replace")

    # ── 1. Direct-call resolution ──────────────────────────────────────
    defined_funcs = set(RE_FUNC_DEF.findall(full_c))
    forward_decls = set(RE_FUNC_FWD.findall(dispatch_c))
    declared_funcs = defined_funcs | forward_decls

    direct_calls = RE_DIRECT_CALL.findall(full_c)
    direct_targets = Counter(c.upper() for c in direct_calls)

    unresolved_calls = sorted(t for t in direct_targets if t not in
                              {f.upper() for f in declared_funcs})
    print(f"[1] direct func_XXXX(cpu) calls: {sum(direct_targets.values())} total, "
          f"{len(direct_targets)} unique targets")
    print(f"    defined functions in full_c: {len(defined_funcs)}")
    print(f"    forward decls in dispatch_c: {len(forward_decls)}")
    print(f"    unresolved direct-call targets: {len(unresolved_calls)}")
    for t in unresolved_calls[:25]:
        print(f"      func_{t}  ({direct_targets[t]} call site{'s' if direct_targets[t]!=1 else ''})")

    # ── 2. Literal call_by_address targets ─────────────────────────────
    # Targets in the declared code regions must resolve to the dispatch
    # table. Targets outside (RAM-loaded overlays, install-at-runtime
    # stubs) are handled by the runtime's dirty-RAM interpreter — expected
    # to be absent from the static dispatch table.
    def is_in_code(vaddr_normalized: int) -> bool:
        for r in cfg.regions:
            phys_lo = r.vaddr_base & cfg.kseg_mask
            phys_hi = phys_lo + (r.rom_end - r.rom_start)
            if phys_lo <= vaddr_normalized < phys_hi:
                return True
        return False

    cba_lits = RE_CALL_BY_ADDR_LIT.findall(full_c)
    cba_lit_set = set(cfg.normalize_addr(int(t, 16)) for t in cba_lits)
    table_entries = RE_DISPATCH_TABLE_ENTRY.findall(dispatch_c)
    table_set = set(cfg.normalize_addr(int(e, 16)) for e in table_entries)

    cba_in_code = {t for t in cba_lit_set if is_in_code(t)}
    cba_in_ram = cba_lit_set - cba_in_code

    cba_missing_in_code = sorted(cba_in_code - table_set)
    print(f"\n[2] literal call_by_address targets: {len(cba_lit_set)} unique, "
          f"{sum(1 for _ in cba_lits)} sites")
    print(f"    dispatch table size: {len(table_set)} entries")
    print(f"    targets in declared code regions: {len(cba_in_code)}")
    print(f"    targets in RAM (dirty-RAM interpreter domain): {len(cba_in_ram)}")
    print(f"    in-code targets MISSING from dispatch: {len(cba_missing_in_code)}")
    for m in cba_missing_in_code[:25]:
        cnt = sum(1 for t in cba_lits if cfg.normalize_addr(int(t, 16)) == m)
        print(f"      0x{m:08X}  ({cnt} site{'s' if cnt!=1 else ''})")

    # ── 3. Indirect call_by_address distribution ───────────────────────
    cba_regs = RE_CALL_BY_ADDR_REG.findall(full_c)
    print(f"\n[3] indirect call_by_address sites: {len(cba_regs)} total")
    print(f"    by register: {dict(Counter(cba_regs).most_common(8))}")

    # ── 4. Tail-call targets ───────────────────────────────────────────
    tail_calls = RE_TAIL_CALL.findall(full_c)
    tail_set = set(cfg.normalize_addr(int(t, 16)) for t in tail_calls)
    tail_missing = sorted(tail_set - table_set)
    print(f"\n[4] tail-call (cpu->pc=...; return;) targets: {len(tail_set)} unique, "
          f"{len(tail_calls)} sites")
    print(f"    tail-call targets MISSING from dispatch: {len(tail_missing)}")
    for m in tail_missing[:25]:
        cnt = sum(1 for t in tail_calls if cfg.normalize_addr(int(t, 16)) == m)
        print(f"      0x{m:08X}  ({cnt} site{'s' if cnt!=1 else ''})")

    # ── 5. Branch-condition population ─────────────────────────────────
    bc_decls = RE_BC_DECL.findall(full_c)
    print(f"\n[5] branch-condition declarations (`int _bc_X = (...);`): {len(bc_decls)}")

    # ── 6. goto block_X target resolution ──────────────────────────────
    labels = set(RE_BLOCK_LABEL.findall(full_c))
    gotos = RE_GOTO_BLOCK.findall(full_c)
    unresolved_gotos = sorted(set(gotos) - labels)
    print(f"\n[6] goto block_X targets: {len(gotos)} sites, {len(set(gotos))} unique")
    print(f"    block labels defined: {len(labels)}")
    print(f"    goto targets WITHOUT a matching block label: {len(unresolved_gotos)}")
    for g in unresolved_gotos[:25]:
        print(f"      {g}")

    # ── Summary ────────────────────────────────────────────────────────
    real_bugs = (
        len(unresolved_calls)        # direct call to undefined function
        + len(cba_missing_in_code)    # in-code literal call_by_address not in dispatch
        + len(tail_missing)           # tail call not in dispatch
        + len(unresolved_gotos)       # goto to undefined label
    )

    print(f"\n[summary] {cfg.name}")
    print(f"  unresolved direct calls           : {len(unresolved_calls)}")
    print(f"  in-code call_by_address misses    : {len(cba_missing_in_code)}")
    print(f"  RAM call_by_address (expected)    : {len(cba_in_ram)}")
    print(f"  tail-call misses                  : {len(tail_missing)}")
    print(f"  unresolved goto labels            : {len(unresolved_gotos)}")
    print(f"  total real-bug findings           : {real_bugs}")
    print(f"  STATUS                            : {'CLEAN' if real_bugs == 0 else 'ISSUES FOUND'}")

    return 0 if real_bugs == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
