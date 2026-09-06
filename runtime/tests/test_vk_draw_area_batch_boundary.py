#!/usr/bin/env python3
from pathlib import Path
import re


source = (Path(__file__).parents[1] / "src" / "gpu_vk_renderer.c").read_text()
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
