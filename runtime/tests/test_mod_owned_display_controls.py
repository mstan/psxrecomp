#!/usr/bin/env python3
"""Guard ownership of PSX display and FMV controls."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

def has_const_bool(name: str, value: bool) -> bool:
    return re.search(
        rf"\bconstexpr\s+bool\s+{name}\s*=\s*{'true' if value else 'false'}\s*;",
        MAIN,
    ) is not None


# Display shape/interpolation remain mod-owned. WipEout explicitly exposes its
# tested FMV-skip option through the launcher, so that capability is product-owned.
for capability in ("ws_offered", "ws_ultrawide_offered", "frame_interpolation_offered"):
    assert has_const_bool(capability, False), (
        f"trusted-mod display capability must remain hidden: {capability}"
    )
assert has_const_bool("skip_fmv_offered", True), "FMV skip must remain launcher-visible"

for legacy_route in (
    "ws_offered = gc.ws_offered;",
    "ws_ultrawide_offered = gc.ws_ultrawide_offered;",
    "frame_interpolation_offered =\n                gc.runtime.video_offer_frame_interpolation;",
    "skip_fmv_offered = gc.runtime.video_offer_skip_fmv;",
):
    assert legacy_route not in MAIN, f"legacy offer flag still controls UI: {legacy_route}"

for hidden_capability in (
    r"gi->widescreen_supported\s*=\s*0\s*;",
    r"gi->aspect_mask\s*=\s*0\s*;",
):
    assert re.search(hidden_capability, MAIN)

for trusted_api in (
    "psx_mod_set_fixed_display_aspect",
    "psx_mod_set_adaptive_display_aspect",
    "psx_mod_set_frame_interpolation",
    "psx_mod_set_auto_skip_fmv",
):
    assert trusted_api in HEADER, f"missing trusted mod API: {trusted_api}"

# Session defaults (including the launcher-owned FMV choice) are restored before
# plugins activate and are allowed to override them.
restore = MAIN.index("g_auto_skip_fmv = defaults.auto_skip_fmv;")
activate = MAIN.index("mod_runtime_activate_plugins();", restore)
assert restore < activate

print("PSX display/FMV ownership guard passed")
