#!/usr/bin/env python3
"""Keep setup-host BIOS registry generation valid ISO C."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNTIME_CMAKE = (ROOT / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

guard = "if(NOT _psxrt_registry_entries)"
sentinel = 'set(_psxrt_registry_entries "    0, /* unused: registry count is zero */'
writer = 'set(_psxrt_registry_c "${CMAKE_BINARY_DIR}/psx_bios_registry.c")'

assert guard in RUNTIME_CMAKE
assert sentinel in RUNTIME_CMAKE
assert RUNTIME_CMAKE.index(guard) < RUNTIME_CMAKE.index(writer)

print("zero-entry BIOS registry guard passed")
