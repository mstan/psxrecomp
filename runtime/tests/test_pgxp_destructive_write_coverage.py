#!/usr/bin/env python3
"""Structural integration guard for PGXP destructive-write invalidation."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def require_guard(source, marker, hook, window=800):
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing instruction path: {marker}")
    if hook not in source[start:start + window]:
        raise AssertionError(f"{marker} does not invalidate with {hook}")


def main():
    oracle = (ROOT / "src/psx_interpreter.c").read_text(encoding="utf-8")
    dirty = (ROOT / "src/dirty_ram_interp.c").read_text(encoding="utf-8")
    memory = (ROOT / "src/memory.c").read_text(encoding="utf-8")
    hooks = (ROOT / "include/pgxp_hooks.h").read_text(encoding="utf-8")

    # The small oracle and the dirty-RAM fallback are independent execution
    # engines. A generated-code-only fix leaves either one able to preserve a
    # byte-identical stale GPR shadow.
    oracle_cases = (
        ("case 0x09: { /* JALR */", "PGXP_GPR_WRITE(rd);", 300),
        ("case 0x24: set_reg(cpu, rd, rs_val & rt_val);", "PGXP_GPR_WRITE(rd);", 180),
        ("case 0x26: set_reg(cpu, rd, rs_val ^ rt_val);", "PGXP_GPR_WRITE(rd);", 180),
        ("case 0x27: set_reg(cpu, rd, ~(rs_val | rt_val));", "PGXP_GPR_WRITE(rd);", 180),
        ("case 0x2A: set_reg(cpu, rd,", "PGXP_GPR_WRITE(rd);", 220),
        ("case 0x2B: set_reg(cpu, rd,", "PGXP_GPR_WRITE(rd);", 220),
        ("case 0x0A: set_reg(cpu, RT(insn),", "PGXP_GPR_WRITE(RT(insn));", 180),
        ("case 0x0B: set_reg(cpu, RT(insn),", "PGXP_GPR_WRITE(RT(insn));", 180),
        ("case 0x0C: set_reg(cpu, RT(insn),", "PGXP_GPR_WRITE(RT(insn));", 180),
        ("case 0x0E: set_reg(cpu, RT(insn),", "PGXP_GPR_WRITE(RT(insn));", 180),
        ("case 0x03: { /* JAL */", "PGXP_GPR_WRITE(31);", 300),
        ("if (link) { set_reg(cpu, 31, pc + 8);", "PGXP_GPR_WRITE(31);", 120),
        ("case 0x00: /* MFC0 */", "psx_pgxp_load_delayed(cpu, insn", 420),
    )
    for marker, hook, window in oracle_cases:
        require_guard(oracle, marker, hook, window)

    # The interpreter's load-delay register and shadow must commit at the same
    # boundary.  Check every ordinary load family plus unaligned merge loads;
    # a source-only recompiler fix would leave this oracle one instruction
    # early and make the dependent successor see the new projection.
    apply = oracle[oracle.find("static inline void apply_load_delay"):
                   oracle.find("static inline void apply_load_delay") + 800]
    if ("cpu->gpr[reg] = value;" not in apply or
            "psx_pgxp_load_commit(cpu, reg, value);" not in apply or
            apply.find("cpu->gpr[reg] = value;") >
            apply.find("psx_pgxp_load_commit(cpu, reg, value);")):
        raise AssertionError("interpreter load-delay commit is not after CPU writeback")
    for marker in (
            "case 0x20: { /* LB */", "case 0x21: { /* LH */",
            "case 0x22: { /* LWL */", "case 0x23: { /* LW */",
            "case 0x24: { /* LBU */", "case 0x25: { /* LHU */",
            "case 0x26: { /* LWR */"):
        require_guard(oracle, marker, "psx_pgxp_load_delayed(cpu, insn", 900)

    dirty_cases = (
        ("case 0x09: { /* JALR rd, rs */", "PGXP_GPR_WRITE(rd ? rd : 31);", 420),
        ("case 0x24: /* AND */", "PGXP_GPR_WRITE(rd);", 260),
        ("case 0x26: /* XOR */", "PGXP_GPR_WRITE(rd);", 260),
        ("case 0x27: /* NOR */", "PGXP_GPR_WRITE(rd);", 260),
        ("case 0x2A: /* SLT */", "PGXP_GPR_WRITE(rd);", 1800),
        ("case 0x2B: /* SLTU */", "PGXP_GPR_WRITE(rd);", 900),
        ("case 0x0A: /* SLTI */", "PGXP_GPR_WRITE(rt);", 2300),
        ("case 0x0B: /* SLTIU */", "PGXP_GPR_WRITE(rt);", 1800),
        ("case 0x0C: /* ANDI */", "PGXP_GPR_WRITE(rt);", 260),
        ("case 0x0E: /* XORI */", "PGXP_GPR_WRITE(rt);", 260),
        ("case 0x03: { /* JAL target */", "PGXP_GPR_WRITE(31);", 380),
        ("case 0x10: /* BLTZAL */", "PGXP_GPR_WRITE(31);", 260),
        ("case 0x11: /* BGEZAL */", "PGXP_GPR_WRITE(31);", 260),
        ("if (cop_op == 0x00) { /* MFC0", "PGXP_GPR_WRITE(rt);", 500),
        ("if (cop_op == 0x02) { /* CFC0", "PGXP_GPR_WRITE(rt);", 500),
    )
    for marker, hook, window in dirty_cases:
        require_guard(dirty, marker, hook, window)

    # All six central RAM/scratch write paths must invalidate before the real
    # store, but only on a page known to contain live projection provenance.
    gate = "if (g_pgxp_memory_armed && pgxp_memory_page_armed(phys))"
    if memory.count(gate) < 6 or memory.count("pgxp_memory_write(phys, 4u);") < 2 or \
            memory.count("pgxp_memory_write(phys, 2u);") < 2 or \
            memory.count("pgxp_memory_write(phys, 1u);") < 2:
        raise AssertionError("RAM/scratch raw-write invalidation coverage is incomplete")

    # In-process generated/interpreted destructive writes pay no callback when
    # the destination has no live shadow, matching the existing ALU gate model.
    for token in ("g_pgxp_gpr_live_mask", "if (_pgxr != 0u &&",
                  "psx_pgxp_gpr_write(cpu, _pgxr)"):
        if token not in hooks:
            raise AssertionError(f"live-destination GPR gate lost: {token}")

    print("PASS: PGXP destructive-write invalidation covers both interpreters and raw memory")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
