#!/usr/bin/env python3
"""Guard host-refresh sanitization + half-rate present self-heal."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

assert "sanitize_host_refresh_hz" in MAIN, (
    "host refresh must be sanitized before cadence selection"
)
assert "matches mode width" in MAIN, (
    "4K width mistaken for Hz (3840) must be rejected"
)
assert "refresh_rate_numerator" in MAIN, (
    "SDL3 rational refresh must be preferred over float refresh_rate"
)
assert "present half-rate self-heal" in MAIN, (
    "Wayland double-block (0.50x) must self-heal to vsync-owned cadence"
)
assert "g_present_half_rate_healed" in MAIN

print("host-refresh sanitize + half-rate self-heal guard passed")
