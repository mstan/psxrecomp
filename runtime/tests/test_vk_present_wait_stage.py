#!/usr/bin/env python3
import re
from pathlib import Path

source = (Path(__file__).parents[1] / "src" / "gpu_vk_renderer.c").read_text(
    encoding="utf-8"
)
match = re.search(
    r"static\s+void\s+submit_present\s*\([^;{}]*\)\s*\{(?P<body>.*?)^\}",
    source,
    flags=re.DOTALL | re.MULTILINE,
)
assert match, "submit_present definition not found"
body = match.group("body")
assert re.search(
    r"VkPipelineStageFlags\s+wait_stage\s*=\s*"
    r"VK_PIPELINE_STAGE_TRANSFER_BIT\s*\|\s*"
    r"VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT\s*;",
    body,
), "swapchain acquire must wait before both transfer and color-output work"
assert re.search(r"\.pWaitDstStageMask\s*=\s*&wait_stage\s*;", body)
print("Vulkan present wait-stage test passed")
