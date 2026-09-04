#!/usr/bin/env python3
"""Guard bundled-only BIOS selection against stale hidden player paths."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

# A bundled-only product must bypass both explicit and cached player paths
# before validate_bios_for_launch can produce an impossible mismatch dialog.
assert "const bool bundled_only =" in MAIN
assert "if (!bundled_only) {" in MAIN
assert MAIN.index("if (!bundled_only) {") < MAIN.index(
    "if (!chosen.empty() && std::filesystem::exists(chosen))"
)

# The launcher must derive persistence from linked capabilities, not from the
# active image: psx_bios_image is deliberately zeroed until selection occurs.
assert "const bool bios_choice_supported =" in MAIN
assert "if (bios_choice_supported && ls.bios_path[0])" in MAIN
assert "ls.bios_path[0] && !psx_bios_image.image_bundled" not in MAIN

print("BIOS selection capability guards passed")
