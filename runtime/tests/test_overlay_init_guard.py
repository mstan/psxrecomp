#!/usr/bin/env python3
"""Structural regression test for pre-init overlay-loader entry points.

The public dispatch/candidate/native-call entry points can be reached during
early BIOS initialization, before overlay_loader_init() constructs their index
tables. They must fail closed on s_active before consulting those tables.

Usage: python runtime/tests/test_overlay_init_guard.py
Exit 0 = PASS.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "runtime" / "src" / "overlay_loader.c"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b(?:int|void)\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        raise AssertionError(f"missing function definition: {name}")

    start = match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unterminated function definition: {name}")


def require_before(body: str, guard_pattern: str, state_pattern: str, name: str) -> None:
    guard = re.search(guard_pattern, body)
    state = re.search(state_pattern, body)
    if not guard:
        raise AssertionError(f"{name}: missing pre-init s_active guard")
    if not state:
        raise AssertionError(f"{name}: test could not find protected index access")
    if guard.start() > state.start():
        raise AssertionError(f"{name}: s_active guard occurs after index access")


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")

    dispatch = function_body(source, "overlay_loader_dispatch")
    require_before(
        dispatch,
        r"if\s*\(\s*!s_active\s*\)\s*return\s+0\s*;",
        r"idx_head\s*\(",
        "overlay_loader_dispatch",
    )

    candidate = function_body(source, "overlay_loader_is_candidate")
    require_before(
        candidate,
        r"if\s*\(\s*!s_active\s*\)\s*return\s+0\s*;",
        r"(?:idx_head|exact_entry_has)\s*\(",
        "overlay_loader_is_candidate",
    )

    call_native = function_body(source, "overlay_loader_call_native")
    require_before(
        call_native,
        r"if\s*\(\s*!s_active\s*\|\|\s*!s_native_exec\s*\)\s*return\s+0\s*;",
        r"idx_head\s*\(",
        "overlay_loader_call_native",
    )

    print("PASS: overlay loader entry points fail closed before initialization")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
