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

# GP1(08h) is authoritative only after the guest writes it. At the following
# VBlank boundary, update both sides of the XOR together: wall period and the
# renderer's actual swap interval/present mode.
assert "refresh_live_crtc_cadence();" in MAIN
assert MAIN.index("refresh_live_crtc_cadence();") < MAIN.index(
    "runtime_perf_frame_begin();", MAIN.index("refresh_live_crtc_cadence();")
), "live CRTC cadence must be reconciled before frame pacing/present work"
assert "period != s_present_crtc_period_cycles" in MAIN, (
    "CRTC transitions must use the exact integer GPU period, not tolerance"
)
assert "s_frame_pacer = FramePacer{ 0 };" in MAIN, (
    "a changed CRTC period must discard debt from the old wall clock"
)
assert "cadence pending guest " in MAIN and "GP1(08h) display mode" in MAIN, (
    "startup must not claim the GPU reset default is the title's video standard"
)
assert "CRTC refresh multiplier %u (%s %.3f Hz)" not in MAIN, (
    "the pre-GP1 CRTC banner must stay removed"
)

GPU = (ROOT / "runtime" / "src" / "gpu.c").read_text(encoding="utf-8")
assert "display_mode_programmed = 1;" in GPU
assert "gpu_display_mode_is_programmed()" in MAIN

LIVE_BEGIN = MAIN.index("static void refresh_live_crtc_cadence(void)")
LIVE_END = MAIN.index("static void netplay_host_present_uncap(void)", LIVE_BEGIN)
LIVE = MAIN[LIVE_BEGIN:LIVE_END]
assert "apply_present_cadence();" in LIVE, (
    "a live CRTC transition must update the renderer, not only the wall period"
)
assert "gpu_set_crtc_refresh_multiplier" not in LIVE, (
    "host cadence reconciliation must observe, never overwrite, plugin timing"
)
assert "if (!g_mod_native_vblank_rate)" in LIVE, (
    "an explicit native-VBlank mod must remain the pacing authority"
)
assert (
    "return g_frame_period_ms > 0.0 && !present_vsync_owns_cadence();" in MAIN
), "wall pacing must remain the exact inverse of driver-vsync ownership"
assert "if (g_frame_interpolation)\n        return 0;" in MAIN
assert "if (g_netplay_vsync_forced_off || psx_netplay_active())\n        return 0;" in MAIN

# Pin the reported reproduction and its NTSC counterpart against the existing
# 2% band. These are exact integer CRTC periods before the ownership test.
def cadence(base_cycles: int, multiplier: int, panel_hz: float) -> tuple[float, bool]:
    guest_hz = 33_868_800.0 / (base_cycles // multiplier)
    matches = guest_hz * 0.98 <= panel_hz <= guest_hz * 1.02
    return guest_hz, matches


pal2_hz, pal2_matches = cadence(677_376, 2, 100.0)
ntsc2_hz, ntsc2_matches = cadence(564_480, 2, 100.0)
assert pal2_hz == 100.0 and pal2_matches
assert ntsc2_hz == 120.0 and not ntsc2_matches

print("host-refresh sanitize + Wayland vsync cadence guard passed")
