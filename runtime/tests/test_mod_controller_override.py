#!/usr/bin/env python3
"""Guard trusted-plugin controller presentation lifecycle."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

for symbol in (
    "PSX_MOD_CONTROLLER_ANALOG",
    "PSX_MOD_CONTROLLER_DIGITAL",
    "PSXModControllerInput",
    "PSXModControllerPresentationCallback",
    "psx_mod_set_controller_mode_override",
    "psx_mod_set_controller_presentation_policy",
):
    assert symbol in HEADER, f"missing trusted-plugin controller API: {symbol}"

assert "PSX_MOD_CONTROLLER_HYBRID" not in HEADER

reset_override = "g_mod_controller_mode_override.fill(-1);"
reset_policy = "policy = ModControllerPresentationPolicy{};"
activate = "mod_runtime_activate_plugins();"
apply = "player_mode[i] = g_mod_controller_mode_override[i];"

for snippet in (reset_override, reset_policy, activate, apply):
    assert snippet in MAIN, (
        f"missing controller presentation lifecycle step: {snippet}"
    )

assert MAIN.index(reset_override) < MAIN.index(activate) < MAIN.index(apply)
assert MAIN.index(reset_policy) < MAIN.index(activate) < MAIN.index(apply)
assert "controller_policy_resolve_mode(" in MAIN
assert "controller_policy_resolve_override_mode(" in MAIN

print("mod controller presentation lifecycle guard passed")
