"""Run source-owned readback checks with a hidden real OpenGL context."""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def probe_result_ok(code, stdout, stderr, expect_unbounded=False):
    lines = stdout.rstrip().splitlines()
    summary = re.fullmatch(r"checks=(\d+) failures=(\d+)", lines[-1]) if lines else None
    if not summary or int(summary[1]) == 0:
        return False
    failures = [line for line in stderr.splitlines() if line.startswith("FAIL ")]
    if expect_unbounded:
        return (code == 1 and int(summary[2]) == 1 and
                failures == ["FAIL single-pixel bounded transfer"])
    return code == 0 and int(summary[2]) == 0 and not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler-bin", required=True)
    parser.add_argument("--sdl-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--gl-source")
    parser.add_argument("--fixture", type=pathlib.Path)
    parser.add_argument("--expect-unbounded", action="store_true")
    args = parser.parse_args()
    framework = pathlib.Path(__file__).resolve().parents[2]
    compiler = pathlib.Path(args.compiler_bin).resolve()
    for name in ("gcc.exe", "g++.exe"):
        if not (compiler / name).is_file():
            parser.error(f"--compiler-bin must contain {name}: {compiler}")
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    dest = pathlib.Path(tempfile.mkdtemp(prefix="readback-", dir=output))
    print("Evidence directory:", dest)
    sdl = pathlib.Path(args.sdl_root).resolve()
    env = os.environ.copy()
    env["PATH"] = str(compiler) + os.pathsep + env.get("PATH", "")
    if args.gl_source:
        shutil.copyfile(args.gl_source, dest / "gpu_gl_renderer.c")
    includes = ["-I", str(dest), "-I", str(framework / "runtime/include"),
                "-I", str(framework / "runtime/src"), "-I", str(sdl / "sdl3-src/include")]
    receipt = []

    def run(command):
        command = [str(value) for value in command]
        result = subprocess.run(command, cwd=dest, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", env=env)
        receipt.append({"cmd": command, "exit": result.returncode,
                        "stdout": result.stdout, "stderr": result.stderr})
        (dest / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        print(result.stdout[-600:], result.stderr[-2000:])
        return result

    fixture = args.fixture or framework / "runtime/tests/test_gl_readback_region.c"
    for name, source in [("probe", fixture), ("sw", framework / "runtime/src/gpu_sw_renderer.c")]:
        if run([compiler / "gcc.exe", "-std=c11", "-O2", "-flto", "-DPSX_SDL3=1",
                "-DPSX_NO_DEBUG_TOOLS=1", *includes, "-c", source,
                "-o", dest / (name + ".o")]).returncode:
            return 2
    libraries = ["m", "kernel32", "user32", "gdi32", "winmm", "imm32", "ole32",
                 "oleaut32", "version", "uuid", "advapi32", "setupapi", "shell32",
                 "dinput8", "opengl32"]
    if run([compiler / "g++.exe", "-O2", "-flto", dest / "probe.o", dest / "sw.o",
            sdl / "sdl3-build/libSDL3.a", *["-l" + name for name in libraries],
            "-o", dest / "probe.exe"]).returncode:
        return 2
    for scale in (1, 4):
        result = run([dest / "probe.exe", scale])
        if not probe_result_ok(result.returncode, result.stdout, result.stderr,
                               args.expect_unbounded):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
