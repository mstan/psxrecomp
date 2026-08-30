#!/usr/bin/env python3
"""Regression guards for the self-contained CLI project pipeline."""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def main() -> int:
    cli = (ROOT / "recompiler/src/main_cli.cpp").read_text(encoding="utf-8")
    bios = (ROOT / "recompiler/src/main_bios.cpp").read_text(encoding="utf-8")
    package = (ROOT / "tools/build_cli.py").read_text(encoding="utf-8")
    new_project_cmake = (
        ROOT / "tools/new_project_layout/templates/CMakeLists.txt.in"
    ).read_text(encoding="utf-8")
    runtime_cmake = (ROOT / "runtime/CMakeLists.txt").read_text(encoding="utf-8")
    recompiler_cmake = (ROOT / "recompiler/CMakeLists.txt").read_text(
        encoding="utf-8")

    require(cli, 'bios_config = \\"psxrecomp/bios/{}\\"',
            "generated game.toml does not name its BIOS profile")
    require(cli, '"--config", fs::absolute(profile_destination).string()',
            "CLI BIOS recompilation does not use the selected profile")
    require(cli, '"--rom", options.bios.string()',
            "CLI BIOS recompilation does not pass the user-selected ROM")
    require(cli, '"--out-dir",',
            "CLI BIOS recompilation does not override the generated output")
    require(cli, 'framework_destination / "generated"',
            "BIOS sources are not emitted where runtime.cmake discovers them")
    require(cli, 'set(PSXRECOMP_BIOS_STEMS \\"{}\\"',
            "generated CMake does not select the emitted BIOS backend")
    require(cli, 'set(PSX_RECOMP_UI OFF CACHE BOOL \\"\\" FORCE)',
            "self-contained CLI projects must not require a game-owned "
            "recomp-ui checkout")
    require(cli, 'PSXRecomp runtime is missing:',
            "generated CLI project does not fail clearly when psxrecomp/ "
            "is missing")
    require(cli, 'git submodule update --init --recursive',
            "generated CLI project missing-runtime error lacks the git "
            "submodule recovery command")
    require(cli, '$RuntimeCMake = Join-Path $Root',
            "generated PowerShell build script lacks a runtime.cmake "
            "preflight")
    require(cli, '[ ! -f \\"$ROOT/psxrecomp/runtime/runtime.cmake\\" ]',
            "generated shell build script lacks a runtime.cmake preflight")
    require(new_project_cmake, 'PSXRecomp runtime is missing:',
            "new-project-layout CMake template does not fail clearly when "
            "psxrecomp/ is missing")
    require(new_project_cmake, 'git submodule update --init --recursive',
            "new-project-layout missing-runtime error lacks the git "
            "submodule recovery command")

    copy_pos = cli.find('fmt::print("[1/4] Copying build framework')
    game_pos = cli.find('fmt::print("[2/4] Recompiling game executable')
    if copy_pos < 0 or game_pos < 0 or copy_pos >= game_pos:
        raise AssertionError(
            "framework/profile must be copied before game recompilation")

    for profile in (
            "OpenBIOS.toml", "SCPH1001.toml",
            "SCPH101.toml", "SCPH5552.toml"):
        require(package, f'"{profile}"',
                f"CLI package omits BIOS profile {profile}")
    require(package, 'shutil.copy2(ROOT / ".gitignore", framework)',
            "packaged framework lacks the project-root marker used by profiles")
    require(package, 'ROOT / "bios" / "openbios.bin"',
            "packaged framework omits the redistributable OpenBIOS image")
    require(package, 'ROOT / "bios" / "OpenBIOS.LICENSE"',
            "packaged framework omits the OpenBIOS license")
    require(package, '"-DBUILD_TESTING=ON"',
            "CLI release graph does not enable its boot-path test")

    for name, cmake in (("runtime", runtime_cmake),
                        ("recompiler", recompiler_cmake)):
        option_pos = cmake.find('option(BUILD_TESTING')
        ctest_pos = cmake.find('include(CTest)')
        if option_pos < 0 or ctest_pos < 0 or option_pos >= ctest_pos:
            raise AssertionError(
                f"{name} must default BUILD_TESTING before include(CTest)")
        require(cmake[option_pos:ctest_pos], "OFF)",
                f"{name} standalone builds must default tests off")

    require(bios, 'a == "--rom"',
            "psxrecomp-bios config mode lacks the ROM override")
    require(bios, 'a == "--out-dir"',
            "psxrecomp-bios config mode lacks the output override")

    print("PASS: CLI profiles, test graph, BIOS overrides, and UI isolation agree")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
