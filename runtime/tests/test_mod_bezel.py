#!/usr/bin/env python3
"""Guard bezel artwork as a mod-owned presentation feature."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime/include/mod_plugins.h").read_text(encoding="utf-8")
MOD_RUNTIME = (ROOT / "runtime/src/mod_runtime.cpp").read_text(encoding="utf-8")
BUILTIN = (ROOT / "runtime/src/mod_builtin_bezel.c").read_text(encoding="utf-8")
CONFIG_H = (ROOT / "recompiler/src/config_loader.h").read_text(encoding="utf-8")
CONFIG_CPP = (ROOT / "recompiler/src/config_loader.cpp").read_text(
    encoding="utf-8"
)
MANIFEST = (
    ROOT
    / "mods/builtin/packages/psx.presentation.bezel/1.0.0/manifest.toml"
).read_text(encoding="utf-8")

assert "video_bezel" not in CONFIG_H
assert 'video.contains("bezel")' not in CONFIG_CPP
assert "psx_mod_set_bezel_artwork" in HEADER
assert "psx_mod_current_package_file" not in HEADER
assert "psx_mod_current_resource_path" in HEADER
assert "psx_mod_set_bezel_artwork" in MAIN
assert "g_video_renderer = 1;" in MAIN
assert "mod-owned OpenGL margin artwork" in MAIN
assert "current_plugin" in MOD_RUNTIME
assert 'psx_mod_current_resource_path("artwork"' in BUILTIN
assert 'psx_mod_register_activation_plugin("psx.bezel"' in BUILTIN
assert 'id = "psx.presentation.bezel"' in MANIFEST
assert "default_enabled = false" in MANIFEST
assert "[[resource]]" in MANIFEST
assert 'id = "artwork"' in MANIFEST
assert 'file_patterns = "*.png,*.jpg,*.jpeg,*.bmp"' in MANIFEST
assert 'id = "psx.bezel"' in MANIFEST
assert not (
    ROOT
    / "mods/builtin/packages/psx.presentation.bezel/1.0.0/bezel.png"
).exists()

print("mod-owned bezel guard passed")
