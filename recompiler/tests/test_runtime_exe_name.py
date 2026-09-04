#!/usr/bin/env python3
"""The runtime's executable name has one source of truth: CMake.

runtime.cmake sets OUTPUT_NAME from MAKE_C_IDENTIFIER(WINDOW_TITLE) and writes
that name to psxrecomp_exe_name-<target>.txt. Two consumers used to re-derive
it instead — psxrecomp_cli.py from --exe-name, and the in-runtime self-compiler
from codegen_setup.exe_basename — each applying the same rule to its own copy
of the window title. The rule is identical; the copies drift the moment a game
is renamed, and the build then links <new>.exe while both consumers hunt for
<old>.exe.

That surfaced on Revelations: Persona as "build succeeded but binary missing"
after a build with zero errors — CMake had emitted
Revelations__Persona_Recompiled.exe while the setup host wanted
Revelations_Persona__Recompiled.exe. Same algorithm, colon in a different
place. What this pins:

  1. the published marker wins over a stale exe_basename;
  2. an older build tree with no marker still resolves via the fallback;
  3. when nothing resolves, the error names the executables that ARE present
     instead of implying the compile failed;
  4. a drifted name is reported as a name mismatch, not a build failure.
"""

from pathlib import Path
import importlib.util
import stat
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]

# The two names the wild failure produced, kept verbatim so the regression is
# recognisable if it ever comes back.
BUILT = "Revelations__Persona_Recompiled"      # MAKE_C_IDENTIFIER of the title
STALE = "Revelations_Persona__Recompiled"      # a drifted second copy

failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    if not cond:
        print(f"FAIL: {what}", file=sys.stderr)
        failures += 1


def load_cli():
    spec = importlib.util.spec_from_file_location(
        "psxrecomp_cli", ROOT / "psxrecomp_cli.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    cli = load_cli()

    # runtime.cmake must actually publish the name, or every consumer below is
    # back to guessing.
    cmake = (ROOT / "runtime/runtime.cmake").read_text(encoding="utf-8")
    check("psxrecomp_exe_name-${target}.txt" in cmake,
          "runtime.cmake writes psxrecomp_exe_name-<target>.txt")
    check("file(GENERATE" in cmake and "TARGET_FILE_BASE_NAME:${target}" in cmake,
          "marker uses the final CMake target filename")
    check('MAKE_C_IDENTIFIER "${PSXRT_WINDOW_TITLE}"' in cmake,
          "OUTPUT_NAME still derives from WINDOW_TITLE (marker must match it)")

    with tempfile.TemporaryDirectory() as tmp:
        build = Path(tmp)
        # Executable bit matters: the "what IS present" listing filters on it
        # (or a .exe suffix), the way a real build output would be.
        built_path = build / BUILT
        built_path.write_text("", encoding="utf-8")
        built_path.chmod(built_path.stat().st_mode | stat.S_IXUSR)
        marker = build / "psxrecomp_exe_name-psx-runtime.txt"
        marker.write_text(BUILT + "\n", encoding="utf-8")

        # 1. The case that broke in the wild.
        exe, err = cli._resolve_runtime_exe(build, "psx-runtime", STALE)
        check(exe is not None and exe.name == BUILT,
              "a stale exe_basename resolves via the published marker")
        check(err is None, "no error when the marker resolves it")

        # A correct exe_basename must keep working unchanged.
        exe, _ = cli._resolve_runtime_exe(build, "psx-runtime", BUILT)
        check(exe is not None and exe.name == BUILT,
              "a matching exe_basename still resolves")

        # 2. Build trees predating the marker fall back to the old behaviour.
        marker.unlink()
        exe, _ = cli._resolve_runtime_exe(build, "psx-runtime", BUILT)
        check(exe is not None and exe.name == BUILT,
              "no marker + correct name falls back to the derived name")

        # 3. Genuinely unresolvable: say what is there rather than accusing the
        #    compiler.
        exe, err = cli._resolve_runtime_exe(build, "psx-runtime", STALE)
        check(exe is None, "no marker + stale name cannot resolve")
        check(err is not None and BUILT in err,
              "the error lists the executables actually present")

        # 4. Marker present, binary gone: a name mismatch, reported as one.
        marker.write_text(BUILT + "\n", encoding="utf-8")
        built_path.unlink()
        exe, err = cli._resolve_runtime_exe(build, "psx-runtime", STALE)
        check(exe is None, "a missing binary still fails")
        check(err is not None and "build itself succeeded" in err,
              "a drifted name is not reported as a build failure")
        check(err is not None and BUILT in err and STALE in err,
              "the error names both the built and the expected name")

    if failures:
        print(f"test_runtime_exe_name: {failures} failure(s)", file=sys.stderr)
        return 1
    print("PASS: runtime exe name has a single source of truth")
    return 0


if __name__ == "__main__":
    sys.exit(main())
