"""Guard the CD controller wiring for the timed tray cycle."""

from pathlib import Path
import re


SOURCE = (Path(__file__).parents[1] / "src" / "cdrom.c").read_text(
    encoding="utf-8"
)


def function_body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", SOURCE)
    assert match, f"missing function: {name}"
    start = match.end()
    depth = 1
    cursor = start
    while cursor < len(SOURCE) and depth:
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    assert depth == 0, f"unterminated function: {name}"
    return SOURCE[start : cursor - 1]


reinsert = function_body("debug_force_cd_reinsert")
assert "cdrom_lid_begin_open" in reinsert
assert "s_lid_irq_pending = 1" in reinsert
assert "set_irq(CDIRQ_ACK)" not in reinsert

present = function_body("present_lid_open_irq_if_ready")
assert "response_push(0x08)" in present
assert "set_irq(CDIRQ_ERROR)" in present

has_disc = function_body("has_disc")
assert "cdrom_lid_media_ready" in has_disc

advance = function_body("cdrom_advance")
assert advance.count("process_lid_state()") >= 2

print("PASS: timed CD lid integration guards")
