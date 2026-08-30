#!/usr/bin/env python3
"""Structural guard: FPS title/stderr telemetry stays explicitly opt-in."""

import re
from pathlib import Path

main_cpp = (Path(__file__).parents[1] / "src" / "main.cpp").read_text(
    encoding="utf-8"
)

assert 'std::getenv("PSX_FPS_TELEMETRY")' in main_cpp
assert re.search(
    r"if\s*\(\s*fps_telemetry_enabled\(\)\s*&&\s*!psx_netplay_in_load_barrier\(\)\s*\)",
    main_cpp,
), "FPS sampling must remain behind the opt-in and netplay-barrier gates"
assert re.search(
    r"sdl_window\s*&&\s*s_fps_base_title\.empty\(\)", main_cpp
), "telemetry must capture the stable base title only once"
assert re.search(
    r"!enabled\s*&&\s*sdl_window\s*&&\s*!s_fps_base_title\.empty\(\)", main_cpp
), "disabling telemetry must restore the stable base title"

# Keep the useful distinction between guest vblanks, completed renders, and
# interpolated display output without pinning capitalization or printf layout.
for metric in ("psx_crtc_frame_hz()", "psx_gpu_display_flip_count()", "display_fps"):
    assert metric in main_cpp, f"missing FPS telemetry metric: {metric}"
title_formats = re.findall(r'"([^"\n]*(?:VBlank|Render|Display)[^"\n]*)"', main_cpp)
assert any("VBlank" in fmt and "Render" in fmt for fmt in title_formats)
assert any("Display" in fmt for fmt in title_formats)

print("PASS: FPS telemetry remains opt-in and title-stable")
