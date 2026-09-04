#!/usr/bin/env python3
"""Structural guards for the [runtime] overlay_region_floor key and the bundled-toolchain gcc path.

- config_loader parses `overlay_region_floor` (hex string or integer) into the runtime config;
- main.cpp applies it before the PSX_OVERLAY_REGION_FLOOR env override, with the same kernel-window clamp;
- the bundled overlay_toolchain/ can drive gcc (not only tcc) when a gcc toolchain is on PATH.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOADER_H = (ROOT / "recompiler/src/config_loader.h").read_text(encoding="utf-8")
LOADER_CPP = (ROOT / "recompiler/src/config_loader.cpp").read_text(encoding="utf-8")
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")

assert "has_overlay_region_floor" in LOADER_H and "overlay_region_floor" in LOADER_H
assert 'runtime.contains("overlay_region_floor")' in LOADER_CPP
assert 'parse_hex(v.as_string(), "runtime.overlay_region_floor")' in LOADER_CPP, "hex string form must be accepted"
# Raw integers must be validated at parse time (review on mstan/psxrecomp#242): a
# negative or >32-bit TOML integer must not wrap into a different address, and
# the value must name main RAM above the kernel window.
assert "raw < 0 || raw > 0xFFFFFFFFll" in LOADER_CPP, "integer form must reject wrap-around values"
assert "phys < 0x00010000u || phys >= 0x00200000u" in LOADER_CPP, "floor must be validated against main RAM"
assert LOADER_CPP.count("overlay_region_floor out of range") == 2, "both rejections must fail loud"

cfg = MAIN.index("gc.runtime.has_overlay_region_floor")
env = MAIN.index('std::getenv("PSX_OVERLAY_REGION_FLOOR")')
assert cfg < env, "config key must be applied before the env override so env still wins"
window = MAIN[cfg:cfg + 300]
assert "0x00010000u" in window, "config floor must keep the kernel-window clamp"

assert "build_toolchain_cmd(\"gcc\")" in MAIN, "bundled toolchain must be able to drive gcc"
assert "build_toolchain_cmd(\"tcc\")" in MAIN
assert "(deferred_has_overlay_ac || tk_present)" in MAIN, "gcc availability must count the bundle"
print("overlay_region_floor config + bundled-gcc guards: OK")
