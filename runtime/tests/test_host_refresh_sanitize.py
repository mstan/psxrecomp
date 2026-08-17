#!/usr/bin/env python3
"""Guard host-refresh sanitization + Wayland vsync cadence policy."""

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
assert "host_video_is_wayland" in MAIN, (
    "Wayland must be detected so driver vsync cannot own cadence by default"
)
assert "PSX_WAYLAND_ALLOW_VSYNC" in MAIN, (
    "opt-in escape hatch for Wayland driver vsync must exist"
)
assert "forcing PSX_VSYNC=0" in MAIN, (
    "half-rate self-heal must force immediate swap + wall pacer, "
    "not hand cadence to driver vsync"
)
assert "assuming %.2f Hz panel so driver vsync owns" not in MAIN, (
    "old self-heal that enabled driver vsync ownership must stay gone"
)
assert "g_present_half_rate_healed" in MAIN

print("host-refresh sanitize + Wayland vsync cadence guard passed")
