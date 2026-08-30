#!/usr/bin/env python3
"""Guard mod-owned, bounded load acceleration and its launcher migration."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime/include/mod_plugins.h").read_text(encoding="utf-8")
CONFIG_H = (ROOT / "recompiler/src/config_loader.h").read_text(encoding="utf-8")
CONFIG_CPP = (ROOT / "recompiler/src/config_loader.cpp").read_text(
    encoding="utf-8"
)

assert "psx_mod_set_load_acceleration" in HEADER
# The accepted range was widened from 2..16 to 1..PSX_MOD_LOAD_ACCEL_MAX (the
# bound is there to reject nonsense, not to curate "blessed" speeds), so pin the
# contract by its symbolic bound rather than the old literals.
assert "wall_clock_multiplier accepts 1..PSX_MOD_LOAD_ACCEL_MAX" in HEADER
assert "#define PSX_MOD_LOAD_ACCEL_MAX" in HEADER
assert "zero is the precise/speedrun-safe policy" in HEADER
assert "psx_mod_set_disc_speed" in HEADER
assert "this changes" in HEADER

# The legacy generic Turbo loads switch is retired: acceleration is mod-only.
# Both config keys are still PARSED (so old game.toml/settings.toml load), but
# neither is honoured, and no title can offer the generic switch any more.
assert "bool                  offer_turbo_loads = false;" in CONFIG_H
assert "DEPRECATED AND IGNORED" in CONFIG_H
assert 'runtime.contains("offer_turbo_loads")' in CONFIG_CPP
assert "rt.has_turbo_loads = true;" in CONFIG_CPP
assert "constexpr bool turbo_loads_offered = false;" in MAIN
assert "turbo_loads_offered = gc.runtime.offer_turbo_loads;" not in MAIN
assert "gi->has_turbo_loads = turbo_loads_offered_b ? 1 : 0;" in MAIN
assert "Turbo loads is mod-owned on PSX" in MAIN

# game.toml [runtime] turbo_loads must not enable anything, only warn.
assert "g_turbo_loads_enabled = 1;\n                std::fprintf" not in MAIN
assert '"psxrecomp: game.toml [runtime] %s%s%s is DEPRECATED and "' in MAIN
assert '"Fast Loading (host pacing)\\" mod' in MAIN

# A stale settings.toml value is neither restored nor written back out, so it
# cannot latch: the launcher draws no row for it, which made a persisted value
# both authoritative and unreachable (MegaManX6Recomp#14).
assert "g_turbo_loads_enabled = us.turbo_loads" not in MAIN
assert "settings.toml [video] turbo_loads = true is " in MAIN
assert 'f << "turbo_loads       = "' not in CONFIG_CPP

# The in-game Settings apply path reads a launcher snapshot taken BEFORE mod
# activation, so it must not write either mod-owned global.
assert re.search(
    r"if\s*\(turbo_loads_offered\)\s*g_turbo_loads_enabled\s*=\s*ls\.turbo_loads\s*\?\s*1\s*:\s*0\s*;",
    MAIN,
)
assert re.search(
    r"if\s*\(skip_fmv_offered\)\s*g_auto_skip_fmv\s*=\s*ls\.auto_skip_fmv\s*\?\s*1\s*:\s*0\s*;",
    MAIN,
)

reset = """g_mod_load_wall_multiplier = -1;
    g_mod_load_release_frames = -1;"""
disc_reset = """g_mod_disc_speed_divisor = -1;
    g_mod_disc_instant_rate = -1;"""
disable_mod_owned_baseline = """if (!turbo_loads_offered)
        g_turbo_loads_enabled = 0;"""
activate = "mod_runtime_activate_plugins();"
apply = "g_turbo_load_wall_multiplier = g_mod_load_wall_multiplier;"
assert reset in MAIN
assert disc_reset in MAIN
assert disable_mod_owned_baseline in MAIN
assert apply in MAIN
# The helper now returns early when no plugin claimed acceleration; assignment
# after that negative-sentinel check is the semantic equivalent of the former
# positive conditional.
reset_at = MAIN.index(reset)
disable_at = MAIN.index(disable_mod_owned_baseline, reset_at)
activate_at = MAIN.index(activate, disable_at)
reject_at = MAIN.index("if (g_mod_load_wall_multiplier < 0)", activate_at)
apply_at = MAIN.index(apply, reject_at)
assert reset_at < disable_at < activate_at < reject_at < apply_at

assert "release_run = g_turbo_load_release_frames;" in MAIN
assert "g_frame_period_ms / (double)g_turbo_load_wall_multiplier" in MAIN
assert "if (!manual_turbo_active && !turbo_load_paced && present_should_wall_pace())" in MAIN
assert "if (g_mod_disc_speed_divisor >= 0)" in MAIN

print("mod-owned load acceleration guard passed")
