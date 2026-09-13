"""Isolated configure/build checks: no BIOS, game or network required."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def run(command, success=True):
    p = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=180)
    if success is not None and (p.returncode == 0) != success:
        raise AssertionError(f"unexpected exit {p.returncode}: {command}\n{p.stdout}\n{p.stderr}")
    return p

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", required=True)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--ninja", required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="psx-ipo-") as temp:
        root = Path(temp)
        (root / "first.c").write_text("int other(void); int main(void) { return other() != 42; }\n")
        (root / "second.cpp").write_text('extern "C" int other(void) { return 42; }\n')
        (root / "fake_modules").mkdir()
        (root / "fake_modules/CheckIPOSupported.cmake").write_text('''
function(check_ipo_supported)
    set(_psx_ipo_ok FALSE PARENT_SCOPE)
    set(_psx_ipo_error "synthetic unsupported toolchain" PARENT_SCOPE)
endfunction()
''')
        (root / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.20)
project(IPOSmoke C CXX)
if(SIMULATE_UNSUPPORTED)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fake_modules")
endif()
include("''' + (ROOT / "cmake/psx_runtime_ipo.cmake").as_posix() + '''")
add_executable(subject first.c second.cpp)
add_library(unrelated OBJECT first.c)
if(PARENT_IPO)
    set_property(TARGET subject PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
psxrecomp_apply_runtime_ipo(subject)
get_target_property(base subject INTERPROCEDURAL_OPTIMIZATION)
get_target_property(release subject INTERPROCEDURAL_OPTIMIZATION_RELEASE)
get_target_property(debug subject INTERPROCEDURAL_OPTIMIZATION_DEBUG)
get_target_property(others unrelated INTERPROCEDURAL_OPTIMIZATION_RELEASE)
file(WRITE "${CMAKE_BINARY_DIR}/properties.txt" "${base}|${release}|${debug}|${others}")
''', encoding="utf-8")
        def configure(name, *options, ok=True):
            dest = root / name
            result = run([args.cmake, "-S", root, "-B", dest, "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={args.ninja}", f"-DCMAKE_C_COMPILER={args.cc}",
                f"-DCMAKE_CXX_COMPILER={args.cxx}", "-DCMAKE_BUILD_TYPE=Release", *options], ok)
            return dest, result
        off, _ = configure("off", "-DSIMULATE_UNSUPPORTED=ON")
        assert (off / "properties.txt").read_text() == "base-NOTFOUND|release-NOTFOUND|debug-NOTFOUND|others-NOTFOUND"
        parent, _ = configure("parent", "-DPARENT_IPO=ON", "-DSIMULATE_UNSUPPORTED=ON")
        assert (parent / "properties.txt").read_text().startswith("TRUE|release-NOTFOUND|")
        _, failure = configure("unsupported", "-DPSX_RUNTIME_IPO=ON", "-DSIMULATE_UNSUPPORTED=ON", ok=False)
        assert "PSX_RUNTIME_IPO=ON was requested" in failure.stderr
        on, result = configure("on", "-DPSX_RUNTIME_IPO=ON", ok=None)
        if result.returncode != 0:
            assert "PSX_RUNTIME_IPO=ON was requested" in result.stderr, result.stderr
            print("PASS: default/parent/unsupported guards; SKIP actual IPO link: toolchain reports unsupported")
            return
        assert (on / "properties.txt").read_text() == "FALSE|TRUE|FALSE|others-NOTFOUND"
        build = run([args.cmake, "--build", on, "--verbose"])
        # MSVC and GCC/Clang spell IPO flags differently; this test's explicit
        # Ninja/compiler arguments cover either native toolchain.
        assert any(flag in build.stdout for flag in ("-flto", "/GL", "-GL")), build.stdout
        run([on / ("subject.exe" if os.name == "nt" else "subject")])
        debug, _ = configure("debug", "-DPSX_RUNTIME_IPO=ON", "-DCMAKE_BUILD_TYPE=Debug")
        dbg_build = run([args.cmake, "--build", debug, "--verbose"])
        assert not any(flag in dbg_build.stdout for flag in ("-flto", "/GL", "-GL"))
        print("PASS: default OFF, parent policy, unsupported failure, scoped ON, actual IPO link/run, Debug OFF")

if __name__ == "__main__":
    main()
