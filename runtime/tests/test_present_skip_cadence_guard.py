#!/usr/bin/env python3
"""Every GL present-skip site must be gated on the vsync-cadence query.

The renderer may elide a swap whose front buffer is unchanged. The frontend
calls present once per guest vblank, so eliding a swap also elides whatever
that swap was blocking on. When driver vsync owns the guest cadence the
wall-clock pacer is deliberately not run (they are XOR), which makes the swap
block the ONLY throttle -- and an elided frame is then unthrottled. A game that
presents every other vblank (BoF3, and any other 30 Hz-presenting title) ran at
exactly 2x on a ~60 Hz panel until each skip site consulted this query.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RENDERER = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
PACING = (ROOT / "runtime" / "include" / "frame_pacing.h").read_text(
    encoding="utf-8"
)

# The skip sites are the branches that increment the probe skip counter and
# return without reaching SDL_GL_SwapWindow. Both consult the OSD, the
# interpolation-cadence owner, and the vsync-cadence owner.
skips = RENDERER.count("s_probe_skip++;")
assert skips >= 2, "expected the VRAM and wide-FBO present-skip sites"
assert RENDERER.count("!psx_present_vsync_owns_cadence() &&") == skips, (
    "every present-skip site must be gated on psx_present_vsync_owns_cadence(); "
    "an ungated skip lets the guest free-run when driver vsync owns cadence"
)
assert RENDERER.count("!host_osd_needs_present() &&") == skips
assert RENDERER.count("!gl_renderer_interpolation_owns_cadence()) {") == skips

# The query is declared beside the pacer it is the XOR partner of, and defined
# in the frontend that owns the cadence decision.
assert "int psx_present_vsync_owns_cadence(void);" in PACING
assert 'extern "C" int psx_present_vsync_owns_cadence(void) {' in MAIN
assert "return present_vsync_owns_cadence();" in MAIN

print("present-skip cadence guard passed (%d skip sites gated)" % skips)
