#!/usr/bin/env python3
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
config_h = (root / "recompiler" / "src" / "config_loader.h").read_text(encoding="utf-8")
config_cpp = (root / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")
runtime_cmake = (root / "runtime" / "runtime.cmake").read_text(encoding="utf-8")

assert "runtime/" + "launcher" not in runtime_cmake
assert "Rml" + "Ui" not in runtime_cmake
assert '#include "launcher.h"' not in main

assert re.search(r"\bbool\s+video_offer_vulkan\s*=\s*false\s*;", config_h)
assert re.search(r"\bbool\s+vulkan_offered\s*=\s*false\s*;", config_h)
assert 'video.contains("offer_vulkan")' in config_cpp
assert re.search(
    r"rt\.video_offer_vulkan\s*=\s*toml::find<bool>\(video,\s*\"offer_vulkan\"\)\s*;",
    config_cpp,
)
assert re.search(r"\bbool\s+vulkan_offered\s*=\s*false\s*;", config_cpp)
assert re.search(r"/\*\s*vulkan_offered\s*\*/\s*vulkan_offered", config_cpp)

assert '"Software"' in main
assert '"OpenGL (Recommended)"' in main
assert '"Vulkan"' in main
assert re.search(r"gi->renderer_labels\s*=\s*kPsxRendererLabels\s*;", main)
assert re.search(r"gi->num_renderers\s*=\s*vulkan_offered_b\s*\?\s*3\s*:\s*2\s*;", main)
assert re.search(
    r"ls\.renderer\s*<\s*0\s*\|\|\s*ls\.renderer\s*>\s*\(vulkan_offered\s*\?\s*2\s*:\s*1\)",
    main,
)
assert "settings requested Vulkan, but this game does not" in main

print("Launcher Vulkan-option test passed")
