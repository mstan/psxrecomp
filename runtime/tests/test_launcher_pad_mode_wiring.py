#!/usr/bin/env python3
"""Guard how the launcher-exit pad-mode resolution is WIRED into main().

test_launcher_pad_mode_resolve.cpp proves the helper computes the right mode.
This proves main() actually uses it, at every site, in an order where a bad
mode cannot escape into settings.toml.

The bug this exists for: an Analog-locked title (Ape Escape, game.toml
default_mode = "analog" + lock_mode = true) booted its pad DIGITAL. The
launcher returned 2 for Player 1 -- whose device defaults to keyboard on a
release install -- the readback loop took ls.pad_mode[] verbatim, and
sio_set_pad_analog() then took the 4-byte digital response: right stick dead,
left stick folded onto the D-pad. lock_mode hides the pad-mode selector, so
there was no UI with which to correct it, and the readback also PERSISTED the
wrong mode, making it survive every later launch.

Reading source rather than running a binary is deliberate: the code under test
lives in the middle of main(), between a launcher round-trip and a settings
write, and is not reachable from a unit test. What can be checked statically is
that no call site was left bypassed and that the ordering invariant holds --
which is the class of mistake that produced the bug.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
LAUNCHER_DEVICE = (
    ROOT / "runtime" / "include" / "launcher_device.h"
).read_text(encoding="utf-8")
CONFIG_LOADER = (
    ROOT / "recompiler" / "src" / "config_loader.h"
).read_text(encoding="utf-8")

# ---- the helper exists, and is a pure function of its four inputs ----------
assert "inline int resolve_player_mode_after_launcher(" in LAUNCHER_DEVICE, (
    "resolve_player_mode_after_launcher() is gone from launcher_device.h"
)
for arm in (
    "if (mod_override_mode >= 0) return mod_override_mode;",
    "if (lock_mode) return locked_mode;",
    "return launcher_mode;",
):
    assert arm in LAUNCHER_DEVICE, f"missing resolution arm: {arm}"

# Precedence is positional in the helper body: override, then lock, then the
# launcher's own value. Reordering these silently changes which source of truth
# wins, and both wrong orders are plausible-looking.
assert (
    LAUNCHER_DEVICE.index("if (mod_override_mode >= 0) return mod_override_mode;")
    < LAUNCHER_DEVICE.index("if (lock_mode) return locked_mode;")
    < LAUNCHER_DEVICE.index("return launcher_mode;")
), "resolution precedence must be override > lock > launcher"

# The C++ unit test hard-codes 1/2 rather than dragging config_loader.h's
# dependency chain into a header-only test. Pin the enum so it cannot drift.
assert (
    "enum PadMode { PAD_MODE_ANALOG = 1, PAD_MODE_DIGITAL = 2 };"
    in CONFIG_LOADER
), "PadMode values changed; test_launcher_pad_mode_resolve.cpp hard-codes them"

# ---- both launcher-exit readback loops go through it ----------------------
CALL = "PSXRecompV4::resolve_player_mode_after_launcher("
calls = [i for i in range(len(MAIN)) if MAIN.startswith(CALL, i)]
assert len(calls) == 2, (
    "expected exactly 2 launcher-exit readback sites calling "
    f"resolve_player_mode_after_launcher(), found {len(calls)}. A third "
    "readback path must call it too; a missing one is the original bug."
)

# ---- and none of them still coerces a keyboard seat destructively --------
# The keyboard seat needs no coercion here: effective_player_mode() reports
# DIGITAL for kind == 1 whatever the seat's stored mode says. Overwriting the
# stored mode instead destroyed it durably -- and for a LOCKED title there is
# no selector left to repair it with.
assert "return (int)PSXRecompV4::PAD_MODE_DIGITAL;" in MAIN, (
    "effective_player_mode() no longer short-circuits keyboard seats to "
    "DIGITAL -- the launcher-exit paths rely on it doing so"
)
assert "player_mode[i] = PSXRecompV4::PAD_MODE_DIGITAL;" not in MAIN, (
    "a launcher-exit path is overwriting a seat's configured mode with "
    "DIGITAL again"
)
assert MAIN.count("ls.pad_mode[i] = player_mode[i];") == 2, (
    "the host must seed the launcher with the seat's configured mode "
    "verbatim, at both launcher-entry sites"
)

# ---- the pre-launcher clamp is still the first line of defence -----------
# Launcher-less builds (and a settings.toml written before the game declared
# lock_mode) are covered only by this one.
assert "player_mode[i] = ctrl_locked_mode[i];" in MAIN, (
    "the pre-launcher lock_mode clamp is gone"
)
assert MAIN.index("player_mode[i] = ctrl_locked_mode[i];") < calls[0], (
    "the pre-launcher clamp must run before the launcher round-trip"
)

# ---- resolution happens BEFORE the mode is persisted ---------------------
# There are exactly two seed.p_mode[] writes. The FIRST is pre-launcher: it
# seeds the UserSettings snapshot from player_mode[] as the pre-launcher clamp
# left it, and nothing is on disk yet (save_user_settings runs later). The
# SECOND is inside the first launcher-exit readback loop and is the one that
# must follow resolution. us.p_mode[] is written inside the soft-return loop.
#
# If either write preceded its resolution, settings.toml would latch a mode the
# game does not support -- which is how the original defect outlived the
# session that produced it.
PERSIST_SEED = "seed.p_mode[i] = player_mode[i];"
PERSIST_US = "us.p_mode[i] = player_mode[i];"
assert MAIN.count(PERSIST_SEED) == 2, (
    f"expected 2 seed.p_mode[] writes, found {MAIN.count(PERSIST_SEED)}"
)
assert MAIN.count(PERSIST_US) == 1, (
    f"expected 1 us.p_mode[] write, found {MAIN.count(PERSIST_US)}"
)
seed_writes = [i for i in range(len(MAIN)) if MAIN.startswith(PERSIST_SEED, i)]
assert seed_writes[0] < calls[0], (
    "the pre-launcher seed.p_mode[] write moved after the launcher round-trip"
)
assert calls[0] < seed_writes[1] < calls[1], (
    "the first launcher-exit resolution must precede its seed.p_mode[] write, "
    "and both must precede the soft-return path"
)
assert calls[1] < MAIN.index(PERSIST_US), (
    "the soft-return resolution must precede the us.p_mode[] write"
)

# ---- the mod override still cannot be lost across a soft return ----------
# `goto session_reboot` re-enters the emulator BELOW the block that applies
# g_mod_controller_mode_override, so the soft-return readback is the only place
# an active override can be re-asserted. That is precisely why the resolution
# helper takes it as an argument instead of the clamp being unconditional.
APPLY = "player_mode[i] = g_mod_controller_mode_override[i];"
assert APPLY in MAIN, "the mod controller-mode override apply block is gone"
assert "goto session_reboot;" in MAIN
assert "\nsession_reboot:" in MAIN
assert MAIN.index("\nsession_reboot:") > MAIN.index(APPLY), (
    "session_reboot: moved above the override apply; if it is now BELOW it, "
    "the soft-return re-assert in the readback loop may be redundant -- "
    "re-derive before deleting it"
)
assert MAIN.count("g_mod_controller_mode_override[i])") == 2, (
    "both launcher-exit resolutions must be passed the live override; found "
    f"{MAIN.count('g_mod_controller_mode_override[i])')} such argument(s)"
)
# The direct apply block reads it twice (the >= 0 test and the assignment).
assert MAIN.count("g_mod_controller_mode_override[i]") == 4, (
    "expected 4 g_mod_controller_mode_override[i] reads: the apply block's "
    "guard + assignment, plus one argument per launcher-exit resolution; "
    f"found {MAIN.count('g_mod_controller_mode_override[i]')}"
)

print("launcher pad-mode wiring guard passed")
