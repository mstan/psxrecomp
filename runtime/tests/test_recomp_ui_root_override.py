#!/usr/bin/env python3
"""Guard the caller-selected recomp-ui root against game-root replacement."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNTIME_CMAKE = ROOT / "runtime" / "runtime.cmake"


def main() -> None:
    text = RUNTIME_CMAKE.read_text(encoding="utf-8")
    function_fallback = """if((NOT RECOMP_UI_ROOT OR RECOMP_UI_ROOT STREQUAL \"\")
       AND EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/recomp_ui.cmake\")"""
    module_fallback = """if(PSX_RECOMP_UI AND (NOT RECOMP_UI_ROOT OR RECOMP_UI_ROOT STREQUAL \"\"))
    if(EXISTS \"${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake\")"""
    assert function_fallback in text, (
        "the game-runtime function must preserve an explicit RECOMP_UI_ROOT "
        "before falling back to the game-root recomp-ui"
    )
    assert module_fallback in text, (
        "the runtime module must preserve an explicit RECOMP_UI_ROOT before "
        "falling back to the game-root recomp-ui"
    )


if __name__ == "__main__":
    main()
