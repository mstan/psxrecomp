#!/usr/bin/env python3
from pathlib import Path
import re
import sys


source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parents[1] / "src" / "gpu_vk_renderer.c"
source = source_path.read_text(encoding="utf-8")
setter = re.search(
    r"static void vkb_set_draw_area\(.*?\n\}", source, flags=re.DOTALL
)
assert setter, "vkb_set_draw_area not found"
body = setter.group(0)

tex_flush = body.find("flush_tex_batch();")
geo_flush = body.find("flush_geometry();")
state_write = body.find("s_da_x1 = x1;")

assert tex_flush >= 0, "draw-area change does not drain textured primitives"
assert geo_flush >= 0, "draw-area change does not drain untextured primitives"
assert tex_flush < state_write and geo_flush < state_write, (
    "draw-area state changes before pending primitives are drained"
)

print("Vulkan draw-area batch-boundary test passed")

# Native-wide overlays must retain the same vertical clip as other geometry.
# This source guard is not a substitute for Vulkan driver/pixel qualification.
wide = re.search(r"static void wide_pass_begin\(.*?\n\}", source, flags=re.DOTALL)
overlay = re.search(r"static void wide_overlay_rect\(.*?\n\}", source, flags=re.DOTALL)
assert wide and overlay, "wide render helpers not found"
assert "s_da_y1" in wide[0] and "s_da_y2" in wide[0]
assert "p_vkCmdSetScissor" in wide[0]
assert "wide_pass_begin(cb);" in overlay[0]
assert "p_vkCmdSetScissor" not in overlay[0], "overlay overrides the draw-area Y clip"
print("Vulkan wide-overlay clip source guard passed")
