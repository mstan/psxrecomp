#!/usr/bin/env python3
"""Guard mixed-refresh monitor moves against startup-latched cadence."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def require(needle: str, message: str) -> None:
    if needle not in MAIN:
        raise AssertionError(message)


require(
    "static double        g_guest_frame_period_ms = PSX_FRAME_PERIOD_MS;",
    "guest cadence must be stored separately from host-synced pacing",
)
require(
    "static int           g_host_refresh_display_idx = -2;",
    "host display identity must be tracked across runtime probes",
)
require(
    "static void refresh_host_display_cadence(int force_log, int force_probe)",
    "host display cadence must have a reusable refresh helper",
)
require(
    "const double guest_hz = 1000.0 / g_guest_frame_period_ms;",
    "host-refresh matching must compare against guest cadence, not effective host period",
)
require(
    "g_frame_period_ms = g_guest_frame_period_ms;\n    if (host_refresh_matches_guest_cadence())",
    "mismatched display probes must restore guest pacing before deciding vsync ownership",
)
require(
    "refresh_host_display_cadence(1, 1);",
    "startup must use the shared host display cadence helper",
)
require(
    "refresh_host_display_cadence(0, 0);",
    "runtime must re-probe host display cadence after window moves",
)
require(
    "disp_idx == g_host_refresh_display_idx",
    "runtime probe must detect display index changes",
)
require(
    "now_ms - g_host_refresh_last_probe_ms < 1000ull",
    "runtime probe must still refresh same-display mode changes periodically",
)

if MAIN.count("SDL_GetWindowDisplayIndex(sdl_window)") != 1:
    raise AssertionError("window display probing should have one shared owner")

startup = MAIN.index("refresh_host_display_cadence(1, 1);")
runtime = MAIN.index("refresh_host_display_cadence(0, 0);")
if startup == runtime:
    raise AssertionError("startup and runtime probes must be distinct call sites")

print("mixed-refresh display cadence guard passed")
