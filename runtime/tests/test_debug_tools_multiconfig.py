#!/usr/bin/env python3
"""Verify PSX_DEBUG_TOOLS policy in a Ninja Multi-Config build graph."""

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

LEAN_DEFINE = "PSX_NO_DEBUG_TOOLS=1"


def configure(cmake: str, module: Path, mode: str) -> list[dict[str, str]]:
    with tempfile.TemporaryDirectory(prefix="psx-debug-tools-") as temp:
        source = Path(temp) / "src"
        build = Path(temp) / "build"
        source.mkdir()
        (source / "dummy.c").write_text("int psx_dummy(void) { return 0; }\n")
        module_path = module.as_posix().replace('"', '\\"')
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(debug_tools_contract C)\n"
            "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
            f'include("{module_path}")\n'
            "add_library(psx-runtime STATIC dummy.c)\n"
            "add_library(psx-beetle STATIC dummy.c)\n"
            "psxrecomp_apply_debug_tools(psx-runtime)\n"
            "psxrecomp_apply_debug_tools(psx-beetle)\n"
        )
        command = [
            cmake,
            "-S",
            str(source),
            "-B",
            str(build),
            "-G",
            "Ninja Multi-Config",
            "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo;MinSizeRel",
        ]
        if mode != "AUTO":
            command.append(f"-DPSX_DEBUG_TOOLS={mode}")
        subprocess.run(command, check=True, capture_output=True, text=True)
        return json.loads((build / "compile_commands.json").read_text())


def definition_by_target_and_config(
    commands: list[dict[str, str]],
) -> dict[tuple[str, str], bool]:
    result: dict[tuple[str, str], bool] = {}
    for entry in commands:
        output = entry.get("output", "").replace("\\", "/")
        target = next(
            (name for name in ("psx-runtime", "psx-beetle") if f"/{name}.dir/" in output),
            None,
        )
        config = next(
            (
                name
                for name in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
                if f"/{name}/" in output
            ),
            None,
        )
        if target and config:
            result[(target, config)] = LEAN_DEFINE in entry["command"]
    return result


def assert_policy(cmake: str, module: Path, mode: str) -> None:
    actual = definition_by_target_and_config(configure(cmake, module, mode))
    for target in ("psx-runtime", "psx-beetle"):
        for config in ("Debug", "Release", "RelWithDebInfo", "MinSizeRel"):
            expected = mode == "OFF" or (
                mode == "AUTO" and config in ("Release", "MinSizeRel")
            )
            key = (target, config)
            if key not in actual:
                raise AssertionError(f"missing compile command for {target} {config}")
            if actual[key] != expected:
                raise AssertionError(
                    f"{mode}: {target} {config} lean={actual[key]}, expected {expected}"
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--runtime", type=Path, required=True)
    args = parser.parse_args()

    module = (args.runtime / "debug_tools.cmake").resolve()
    runtime_cmake = (args.runtime / "runtime.cmake").read_text()
    runtime_lists = (args.runtime / "CMakeLists.txt").read_text()
    if "psxrecomp_apply_debug_tools(${target})" not in runtime_cmake:
        raise AssertionError("runtime targets do not use the shared debug-tools policy")
    if "psxrecomp_apply_debug_tools(psx-beetle)" not in runtime_lists:
        raise AssertionError("psx-beetle does not use the shared debug-tools policy")

    for mode in ("AUTO", "ON", "OFF"):
        assert_policy(args.cmake, module, mode)
    print("debug-tools multi-config contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
