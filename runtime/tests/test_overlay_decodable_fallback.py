#!/usr/bin/env python3
"""Structural regression test for unmarked post-EXE code discovery."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "runtime" / "src" / "dirty_ram_interp.c"
HEADER = ROOT / "runtime" / "include" / "dirty_ram_interp.h"
MEMORY = ROOT / "runtime" / "src" / "memory.c"


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\bstatic\s+(?:inline\s+)?int\s+{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.S,
    )
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


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    memory = MEMORY.read_text(encoding="utf-8")
    body = function_body(source, "dirty_ram_dispatch_inner")
    overlay_region = function_body(header, "phys_is_overlay_region")
    baseline = re.search(
        r"\bvoid\s+dirty_ram_clear_image_baseline\s*\([^;]*?\)\s*\{",
        memory,
        re.S,
    )
    if not baseline:
        raise AssertionError("missing function definition: dirty_ram_clear_image_baseline")
    start = baseline.end()
    depth = 1
    for pos in range(start, len(memory)):
        if memory[pos] == "{":
            depth += 1
        elif memory[pos] == "}":
            depth -= 1
            if depth == 0:
                baseline_body = memory[start:pos]
                break
    else:
        raise AssertionError("unterminated function definition: dirty_ram_clear_image_baseline")

    dirty_gate = body.find("!dirty_ram_is_dirty(phys) && !clean_game_text_miss")
    region_gate = body.find("phys_is_overlay_region(phys)", dirty_gate)
    ram_gate = body.find("phys < (2u * 1024u * 1024u)", dirty_gate)
    decode_gate = body.find("dirty_ram_word_looks_decodable(fetch_word(phys))", dirty_gate)
    mark = body.find("dirty_ram_mark_executable_range(phys, 4u)", dirty_gate)
    if min(dirty_gate, region_gate, ram_gate, decode_gate, mark) < 0:
        raise AssertionError("missing dirty/region/RAM/decode/mark fallback chain")
    if not (dirty_gate < region_gate < decode_gate < mark and dirty_gate < ram_gate < mark):
        raise AssertionError("fallback checks or executable marking are out of order")
    if "phys >= g_overlay_region_floor" not in overlay_region:
        raise AssertionError("overlay-region helper lost the above-text floor check")
    if "phys < g_text_image_lo" not in overlay_region:
        raise AssertionError("overlay-region helper lost the below-text high-load check")
    if "uint32_t base = g_text_image_lo" not in baseline_body:
        raise AssertionError("baseline clear no longer starts from text_image_lo")
    if "if (base >= floor) return;" not in baseline_body:
        raise AssertionError("baseline clear no longer guards high-load/empty ranges")

    fallback = body[dirty_gate:mark + 200]
    if "else {\n            return 0;" not in fallback:
        raise AssertionError("invalid/data targets do not fail closed")

    print("PASS: decodable post-EXE dispatch fallback is guarded and fail-closed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
