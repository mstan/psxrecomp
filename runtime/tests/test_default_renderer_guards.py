#!/usr/bin/env python3
"""Guard OpenGL as the default renderer across runtime and generators.

The performance validation path assumes hardware OpenGL unless a user or test
explicitly opts into software. A missing [video] block or stale generator must
not silently fall back to the slow SDL/software renderer.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def require_regex(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.S):
        raise AssertionError(message)


def main() -> int:
    config = (ROOT / "recompiler/src/config_loader.h").read_text(encoding="utf-8")
    main_cpp = (ROOT / "runtime/src/main.cpp").read_text(encoding="utf-8")
    cli = (ROOT / "recompiler/src/main_cli.cpp").read_text(encoding="utf-8")
    toml = (ROOT / "recompiler/src/main_toml.cpp").read_text(encoding="utf-8")

    require(config, "inline constexpr int VIDEO_RENDERER_OPENGL = 1;",
            "OpenGL renderer ID is not centrally named")
    require(config,
            "inline constexpr int DEFAULT_VIDEO_RENDERER = VIDEO_RENDERER_OPENGL;",
            "runtime config default renderer is not OpenGL")
    require(config, "video_renderer = DEFAULT_VIDEO_RENDERER;",
            "RuntimeConfig no longer uses the central default renderer")
    require(main_cpp,
            "g_video_renderer = PSXRecompV4::DEFAULT_VIDEO_RENDERER;",
            "runtime boot default no longer uses the central default renderer")

    generated_renderer = r'\[video\]\\n"\s*"renderer = \\"opengl\\"\\n'
    require_regex(cli, generated_renderer,
                  "psxrecomp CLI-generated game.toml does not force OpenGL")
    require(toml, 'toml += "[video]\\n";',
            "psxrecomp-toml output is missing a [video] block")
    require(toml, 'toml += "renderer = \\"opengl\\"\\n";',
            "psxrecomp-toml output does not force OpenGL")

    print("PASS: OpenGL is the default renderer and generated TOMLs say so")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
