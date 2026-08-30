#!/usr/bin/env python3
"""Guard the OpenGL release build's diagnostic hot-path split."""

import re
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src/gpu_gl_renderer.c"


def configuration_view(source: str, debug_tools: bool) -> str:
    """Resolve only PSX_NO_DEBUG_TOOLS branches, retaining unknown branches."""
    output: list[str] = []
    active = True
    stack: list[tuple[bool, bool, bool]] = []
    for line in source.splitlines():
        directive = line.strip()
        if directive == "#ifndef PSX_NO_DEBUG_TOOLS":
            stack.append((True, active, debug_tools))
            active = active and debug_tools
        elif directive == "#ifdef PSX_NO_DEBUG_TOOLS":
            stack.append((True, active, not debug_tools))
            active = active and not debug_tools
        elif directive.startswith(("#if", "#ifdef", "#ifndef")):
            stack.append((False, active, True))
        elif directive == "#else" and stack:
            target, parent, first = stack[-1]
            if target:
                active = parent and not first
        elif directive == "#endif" and stack:
            _target, parent, _first = stack.pop()
            active = parent
        elif active:
            output.append(line)
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", "\n".join(output), flags=re.DOTALL)


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def forbid(source: str, needle: str, message: str) -> None:
    if needle in source:
        raise AssertionError(message)


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    release = " ".join(configuration_view(source, debug_tools=False).split())
    debug = " ".join(configuration_view(source, debug_tools=True).split())

    for symbol in (
        "s_coh_ring", "s_coh_seq", "s_scene_prims", "s_ptrace", "s_bdg_",
        "s_batch_total", "s_batch_reason",
        "cw_ms", "s_cw_", "s_pf_", "s_mq_", "PFN_glGenQueries",
        "p_glBeginQuery", "p_glEndQuery", "p_glQueryCounter",
    ):
        forbid(release, symbol, f"release GL renderer retains diagnostic state/call: {symbol}")

    require(release,
            "present_dirty_rect(x0, y0, x1, y1, 1);",
            "coherency cleanup removed correctness-critical present dirty marking")
    require(release, "if (s_pc_valid) { x1 += 1; y1 += 1; }",
            "primitive cleanup removed precise-coordinate bbox widening")
    require(release, "rect_add(&s_pack_dirty, x0, y0, x1, y1);",
            "primitive cleanup removed functional pack dirty tracking")
    require(release, "uint64_t gl_renderer_coh_total(void) { return 0; }",
            "release coherency total is not an ABI-compatible zero stub")
    require(release, "if (out_tex_frac) *out_tex_frac = 0.0;",
            "release primitive profiler accessor does not zero its output")
    require(release, "for (int i = 0; i < 18; i++) out[i] = 0.0;",
            "release frame profiler accessor does not zero its output")
    require(release, "for (int i = 0; i < 8; i++) out[i] = 0;",
            "release batch profiler accessor does not zero its output")

    for symbol in ("s_coh_ring", "s_ptrace", "s_scene_prims", "s_batch_reason", "cw_ms",
                   "s_pf_ring", "p_glQueryCounter"):
        require(debug, symbol, f"diagnostics build lost GL instrumentation: {symbol}")

    print("test_gl_release_diag_guards: all checks passed")


if __name__ == "__main__":
    main()
