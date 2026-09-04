#!/usr/bin/env python3
"""Guard the debug hi-res screenshot's byte-stride renderer contract."""

from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parents[1] / "src" / "debug_server.c"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\bstatic\s+void\s+{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        raise AssertionError(f"missing function {name}")
    start, depth = match.end(), 1
    for position in range(start, len(source)):
        depth += source[position] == "{"
        depth -= source[position] == "}"
        if depth == 0:
            return source[start:position]
    raise AssertionError(f"unterminated function {name}")


body = function_body(SOURCE.read_text(encoding="utf-8"), "handle_screenshot_hires")
required = "(int)(ow * sizeof(*argb))"
if body.count(required) != 2:
    raise AssertionError(
        "hi-res and native-fallback screenshot renders must both pass an ARGB byte pitch"
    )
if re.search(r"gr_render_display(?:_hires)?\s*\(\s*argb\s*,\s*\(int\)ow\s*,", body):
    raise AssertionError("screenshot renderer still passes its pitch in pixels")

print("PASS: screenshot_hires uses byte strides for both renderer paths.")
