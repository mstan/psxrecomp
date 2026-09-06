#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
config_h = (root / "recompiler" / "src" / "config_loader.h").read_text(encoding="utf-8")
config_cpp = (root / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

assert "runtime/" + "launcher" not in runtime_cmake
assert "Rml" + "Ui" not in runtime_cmake
assert '#include "launcher.h"' not in main

assert "bool                  video_offer_vulkan = false;" in config_h
assert "bool                  video_offer_vulkan_nographics = true;" in config_h
assert "bool vulkan_offered = false;" in config_h
assert "bool vulkan_nographics_offered = false;" in config_h
assert 'video.contains("offer_vulkan")' in config_cpp
assert 'rt.video_offer_vulkan = toml::find<bool>(video, "offer_vulkan");' in config_cpp
assert 'video.contains("offer_vulkan_nographics")' in config_cpp
assert "bool vulkan_offered = false;" in config_cpp
assert "bool vulkan_nographics_offered = true;" in config_cpp
assert "/*vulkan_offered*/" in config_cpp
assert "/*vulkan_nographics_offered*/" in config_cpp

assert '"Software"' in main
assert '"OpenGL (Recommended)"' in main
assert '"Vulkan (Native)"' in main
assert '"Vulkan (NoGraphicsAPI, Experimental)"' in main
assert "gi->renderer_labels = kPsxRendererLabels;" in main
assert "max_allowed_renderer(vulkan_offered_b," in main
assert "renderer_allowed_by_launcher(ls.renderer, vulkan_offered," in main
assert "settings requested %s, but this game/build does" in main

print("Launcher Vulkan-option test passed")
