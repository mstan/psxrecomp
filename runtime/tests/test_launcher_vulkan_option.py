#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text()
config_h = (root / "recompiler" / "src" / "config_loader.h").read_text()
config_cpp = (root / "recompiler" / "src" / "config_loader.cpp").read_text()
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text()

assert "runtime/" + "launcher" not in runtime_cmake
assert "Rml" + "Ui" not in runtime_cmake
assert '#include "launcher.h"' not in main

assert "bool                  video_offer_vulkan = false;" in config_h
assert "bool vulkan_offered = false;" in config_h
assert 'video.contains("offer_vulkan")' in config_cpp
assert 'rt.video_offer_vulkan = toml::find<bool>(video, "offer_vulkan");' in config_cpp
assert "bool vulkan_offered = false;" in config_cpp
assert "/*vulkan_offered*/" in config_cpp

assert '"Software"' in main
assert '"OpenGL (Recommended)"' in main
assert '"Vulkan"' in main
assert "gi.renderer_labels      = kPsxRendererLabels;" in main
assert "gi.num_renderers        = vulkan_offered ? 3 : 2;" in main
assert "ls.renderer < 0 || ls.renderer > (vulkan_offered ? 2 : 1)" in main
assert "settings requested Vulkan, but this game does not" in main

print("Launcher Vulkan-option test passed")
