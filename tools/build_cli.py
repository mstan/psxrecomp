"""Build and package the PSXRecomp developer command-line tools."""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ("psxrecomp", "psxrecomp-toml", "psxrecomp-game", "psxrecomp-bios")


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def platform_name() -> str:
    systems = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}
    machines = {
        "amd64": "x86_64",
        "x86_64": "x86_64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }
    raw_system = platform.system()
    if raw_system.startswith(("MSYS", "MINGW", "CYGWIN")):
        raw_system = "Windows"
    system = systems.get(raw_system, raw_system.lower())
    machine = machines.get(platform.machine().lower(), platform.machine().lower())
    return f"{system}-{machine}"


def find_tool(build_dir: pathlib.Path, name: str, config: str) -> pathlib.Path:
    suffix = ".exe" if os.name == "nt" else ""
    filename = name + suffix
    candidates = (build_dir / filename, build_dir / config / filename)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    checked = ", ".join(str(path) for path in candidates)
    raise FileNotFoundError(f"{filename} was not built; checked {checked}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="build a redistributable PSXRecomp CLI archive")
    parser.add_argument(
        "configuration", nargs="?", choices=("release", "debug"),
        default="release", help="build configuration (default: release)")
    parser.add_argument(
        "--build-dir", default="recompiler/build-cli",
        help="CMake build directory (default: recompiler/build-cli)")
    parser.add_argument(
        "--dist-dir", default="dist",
        help="package output directory (default: dist)")
    parser.add_argument(
        "--skip-build", action="store_true",
        help="package tools already present in --build-dir")
    args = parser.parse_args()

    config = "Release" if args.configuration == "release" else "Debug"
    build_dir = (ROOT / args.build_dir).resolve()
    dist_dir = (ROOT / args.dist_dir).resolve()

    if not args.skip_build:
        if shutil.which("cmake") is None:
            parser.error("CMake is not installed or is not on PATH")
        source_arg = os.path.relpath(ROOT / "recompiler", ROOT)
        build_arg = os.path.relpath(build_dir, ROOT)
        configure = [
            "cmake", "-S", source_arg, "-B", build_arg,
            f"-DCMAKE_BUILD_TYPE={config}",
            "-DPSXRECOMP_STATIC_CLI=ON",
        ]
        if not (build_dir / "CMakeCache.txt").exists() and shutil.which("ninja"):
            configure += ["-G", "Ninja"]
        run(configure)
        jobs = str(os.cpu_count() or 2)
        run([
            "cmake", "--build", build_arg, "--config", config,
            "--target", *TOOLS, "--parallel", jobs,
        ])

    package = f"psxrecomp-cli-{platform_name()}"
    stage = dist_dir / package
    archive = dist_dir / f"{package}.zip"
    dist_dir.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        shutil.rmtree(stage)
    if archive.exists():
        archive.unlink()
    stage.mkdir()

    shutil.copy2(find_tool(build_dir, "psxrecomp", config), stage)
    libexec = stage / "libexec"
    libexec.mkdir()
    for name in TOOLS[1:]:
        shutil.copy2(find_tool(build_dir, name, config), libexec)
    share = stage / "share"
    share.mkdir()
    shutil.copy2(
        ROOT / "recompiler" / "seeds" / "phase2_ghidra_seeds.json", share)

    framework = stage / "framework"
    source_ignore = shutil.ignore_patterns(
        "build", "build-*", "tests", "__pycache__", "*.pyc", "*.pyo",
        "*.exe", "*.obj", "*.pdb", "*.lib")
    shutil.copytree(ROOT / "runtime", framework / "runtime", ignore=source_ignore)
    shutil.copytree(ROOT / "recompiler", framework / "recompiler", ignore=source_ignore)
    shutil.copy2(ROOT / "LICENSE", framework)
    shutil.copy2(ROOT / "THIRD_PARTY_ATTRIBUTION.md", framework)
    shutil.copy2(ROOT / "README.md", stage)
    shutil.copy2(ROOT / "LICENSE", stage)
    shutil.copy2(ROOT / "THIRD_PARTY_ATTRIBUTION.md", stage)

    shutil.make_archive(str(archive.with_suffix("")), "zip", dist_dir, package)
    print(f"PSXRecomp CLI package: {archive}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"build_cli: command failed with exit code {exc.returncode}",
              file=sys.stderr)
        raise SystemExit(exc.returncode)
