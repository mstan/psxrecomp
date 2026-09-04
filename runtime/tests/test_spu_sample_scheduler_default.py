"""Guard the title-neutral SPU IRQ scheduling correction.

SPU RAM IRQ-address hits can occur more than once per video frame.  The runtime
must let guest code acknowledge and re-arm IRQ9 between 44.1-kHz samples rather
than rendering a VBlank-sized chunk and collapsing the hits into one latch.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
INTERRUPTS = (ROOT / "runtime" / "src" / "interrupts.c").read_text(encoding="utf-8")
CYCLES = (ROOT / "runtime" / "src" / "psx_cycles.c").read_text(encoding="utf-8")
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

# Production defaults to faithful per-sample scheduling.  Keep only an explicit
# zero-valued diagnostic opt-out for comparing old captures.
assert 'getenv("PSX_SPU_SAMPLE_EVENTS")' in INTERRUPTS
assert 'enabled = (!e || !*e || strcmp(e, "0") != 0) ? 1 : 0;' in INTERRUPTS

# The sample boundary must participate in the common device-deadline slicer and
# be serviced after each charged slice, otherwise the default above is inert.
assert "psx_spu_sample_event_cycles_to_next()" in CYCLES
assert "psx_spu_sample_event_service();" in CYCLES

# Register and prime the SPU pump in both the visible and headless frontends.
# A frame-count-only headless smoke must exercise the same sample epoch as the
# operator build, and the first service must not establish a shifted epoch.
prime = re.compile(
    r"psx_set_midframe_audio_pump\(sdl_audio_pump_midframe\);"
    r".{0,500}?sdl_audio_pump_midframe\(\);",
    re.DOTALL,
)
assert len(prime.findall(MAIN)) >= 2
assert MAIN.index("if (g_headless) {\n    audio_trace_init();") < MAIN.index(
    'std::fprintf(stdout, "psxrecomp: headless frontend enabled\\n");'
)

# One hardware SPU output sample is exactly 768 CPU cycles.
assert "psx_get_cycle_count() % 768u" in INTERRUPTS
