#!/usr/bin/env python3
"""Guard queued texture upload order when OpenGL leaves depth24 scanout."""

from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parents[1] / "src" / "gpu_gl_renderer.c"


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


body = function_body(SOURCE.read_text(encoding="utf-8"), "depth24_upload_policy")
leave = body.index("else if (!d24 && s_depth24_skip_up)")
leave_body = body[leave:]
clear = leave_body.index("depth24_clear_skipped_fb();")
flush = leave_body.index("flush_cpu_upload();")
reset = leave_body.index("gpu_depth24_upload_span_reset();")

if not clear < flush < reset:
    raise AssertionError("depth24 exit must clear the movie band before queued uploads land")
if re.search(r"\bs_up_nrects\s*=\s*0\s*;", leave_body[:reset]):
    raise AssertionError("depth24 exit must not discard queued texture uploads")

print("PASS: OpenGL depth24 exit preserves queued texture upload order.")
