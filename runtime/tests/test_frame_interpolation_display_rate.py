#!/usr/bin/env python3
"""Guard the mod API's zero sentinel: follow the measured display rate."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

EXPECTED_ASSIGNMENT = """g_frame_interpolation_fps =
        frames_per_second ? (int)frames_per_second : 0;"""

assert EXPECTED_ASSIGNMENT in MAIN, (
    "psx_mod_set_frame_interpolation(0) must preserve the 0 sentinel so "
    "gl_renderer_set_interpolation resolves it to the measured host refresh"
)
assert "frames_per_second ? (int)frames_per_second : -1" not in MAIN
assert "zero follows the measured host-display refresh rate" in HEADER, (
    "the public plugin contract must document the display-rate sentinel"
)

print("frame interpolation display-rate sentinel guard passed")
