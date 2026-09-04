#!/usr/bin/env python3
"""Keep dirty-text continuation handoff behavior from regressing."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    memory = (root / "runtime/src/memory.c").read_text(encoding="utf-8")
    interp = (root / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    compat = (root / "runtime/src/game_dispatch_compat.c").read_text(encoding="utf-8")
    runtime_cmake = (root / "runtime/runtime.cmake").read_text(encoding="utf-8")

    start = memory.index("int dirty_ram_text_native_ok_ranges_from(")
    end = memory.index("\n/* Preserve the generated-code ABI", start)
    range_guard = memory[start:end]

    required_range_fragments = (
        "uint32_t exec_pc",
        "if (phys + len <= at) continue;",
        "len -= at - phys;",
        "if (!any)",
    )
    for fragment in required_range_fragments:
        if fragment not in range_guard:
            raise AssertionError(f"missing continuation range guard: {fragment}")
    if "text_diverged_bitmap" in range_guard:
        raise AssertionError("exact-range mismatch still sticky-poisons a whole page")
    if "dirty_ram_text_native_ok_ranges_from(lo_len_pairs, count, 0u)" not in memory:
        raise AssertionError("legacy generated-code ABI is not preserved")

    handoff = "clean_game_text_miss && interp_enter_compiled(cpu, "
    if interp.count(handoff) != 1:
        raise AssertionError("expected one suffix-validated transfer handoff")
    if "interp_enter_compiled(cpu, target)" not in interp:
        raise AssertionError("missing transfer-boundary continuation handoff")
    if "psx_game_text_native_ok_full(pc) &&" not in interp:
        raise AssertionError("straight-line handoff lacks full-range validation")
    if "interp_enter_compiled(cpu, pc)" not in interp:
        raise AssertionError("missing call-return continuation handoff")
    if "PSX_GAME_DISPATCH_HAS_NATIVE_OK_FULL" not in compat:
        raise AssertionError("older generated dispatchers lost full-guard compatibility")
    if compat.count("int psx_game_text_native_ok_full(uint32_t addr)") != 3:
        raise AssertionError("full-guard compatibility branches are incomplete")
    if compat.count("int psx_game_text_native_ok_full(uint32_t addr)\n{\n    (void)addr;\n    return 0;\n}") != 3:
        raise AssertionError("a dispatcher without the full ABI may accept an unsafe handoff")
    if "has_game_dispatch_native_ok_full" not in runtime_cmake:
        raise AssertionError("runtime does not detect the generated full-range ABI")

    print("dirty-text continuation guards: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
