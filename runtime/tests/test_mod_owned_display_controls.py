#!/usr/bin/env python3
"""Guard PSX display enhancements as trusted-mod-only features."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

for declaration in (
    "constexpr bool ws_offered = false;",
    "constexpr bool ws_ultrawide_offered = false;",
    "constexpr bool frame_interpolation_offered = false;",
    "constexpr bool skip_fmv_offered = false;",
):
    assert declaration in MAIN, f"PSX launcher capability must default off: {declaration}"

for legacy_route in (
    "ws_offered = gc.ws_offered;",
    "ws_ultrawide_offered = gc.ws_ultrawide_offered;",
    "frame_interpolation_offered =\n                gc.runtime.video_offer_frame_interpolation;",
    "skip_fmv_offered = gc.runtime.video_offer_skip_fmv;",
):
    assert legacy_route not in MAIN, f"legacy offer flag still controls UI: {legacy_route}"

for hidden_capability in (
    "gi->widescreen_supported = 0;",
    "gi->aspect_mask = 0;",
):
    assert hidden_capability in MAIN

for trusted_api in (
    "psx_mod_set_fixed_display_aspect",
    "psx_mod_set_adaptive_display_aspect",
    "psx_mod_set_frame_interpolation",
    "psx_mod_set_auto_skip_fmv",
):
    assert trusted_api in HEADER, f"missing trusted mod API: {trusted_api}"

assert MAIN.index("g_auto_skip_fmv = 0;") < MAIN.index("mod_runtime_activate_plugins();")

print("mod-owned PSX display controls guard passed")
