#!/usr/bin/env python3
"""Guard PAL timing against 60 Hz host-refresh cadence leakage."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")


def require(needle: str, message: str) -> None:
    if needle not in MAIN:
        raise AssertionError(message)


require(
    "static int host_refresh_matches_guest_cadence(void)",
    "host refresh must be compared against the active guest cadence",
)
require(
    "const double guest_hz = 1000.0 / g_guest_frame_period_ms;",
    "guest cadence must derive from the current frame period",
)
require(
    "return std::fabs(g_host_refresh_hz - guest_hz) <= guest_hz * 0.02;",
    "host refresh tolerance must be relative to the current guest cadence",
)
require(
    "if (host_refresh_matches_guest_cadence())",
    "host-refresh sync must not overwrite PAL timing from a hard-coded 60 Hz check",
)
require(
    "return host_refresh_matches_guest_cadence();",
    "driver vsync cadence ownership must respect the active guest cadence",
)

for forbidden in (
    "host_refresh_is_approx_60hz",
    "host_hz >= 58.8",
    "g_host_refresh_hz >= 58.8",
    "host_hz <= 61.2",
    "g_host_refresh_hz <= 61.2",
):
    if forbidden in MAIN:
        raise AssertionError(
            f"PAL timing can leak back to the old 60 Hz-only cadence check: {forbidden}"
        )

print("PAL host-refresh cadence guard passed")
