#!/usr/bin/env python3
"""Verify lean fntrace keeps its API without reserving the diagnostic ring."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from test_cd_spu_trace_config import is_msvc, object_info, require

MIB = 1024 * 1024
PUBLIC_SYMBOLS = {
    "fntrace_record",
    "fntrace_set_game_range",
    "fntrace_is_game_started",
    "fntrace_mark_game_started",
    "fntrace_maybe_mark_game_started",
    "fntrace_arm",
    "fntrace_arm_clear",
    "fntrace_arm_count",
    "fntrace_arm_get",
    "fntrace_arm_from_env",
    "fntrace_clear",
    "fntrace_total",
    "fntrace_get",
}


def compile_object(compiler: Path, source: Path, include_dir: Path,
                   lean: bool, output: Path) -> None:
    if is_msvc(compiler):
        define = ["/DPSX_NO_DEBUG_TOOLS=1"] if lean else []
        command = [str(compiler), "/nologo", "/std:c11", "/Od", *define,
                   f"/I{include_dir}", "/c", str(source), f"/Fo{output}"]
    else:
        define = ["-DPSX_NO_DEBUG_TOOLS=1"] if lean else []
        command = [str(compiler), "-std=c99", "-O0", *define,
                   f"-I{include_dir}", "-c", str(source), "-o", str(output)]
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    source = runtime / "src/fntrace.c"
    header = (runtime / "include/fntrace.h").read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    require("#ifndef PSX_NO_DEBUG_TOOLS\nPSX_BSS FntraceEntry "
            "g_fntrace_ring[FNTRACE_RING_CAP];" in source_text,
            "fntrace storage is not debug-only")
    require("uint64_t fntrace_total(void);" in header,
            "fntrace total accessor is not public")
    require("const FntraceEntry* fntrace_get(uint64_t seq);" in header,
            "fntrace entry accessor is not public")

    with tempfile.TemporaryDirectory(prefix="fntrace-config-") as temp_name:
        temp = Path(temp_name)
        suffix = ".obj" if is_msvc(args.compiler) else ".o"
        debug_object = temp / f"fntrace-debug{suffix}"
        lean_object = temp / f"fntrace-lean{suffix}"
        compile_object(args.compiler, source, runtime / "include", False,
                       debug_object)
        compile_object(args.compiler, source, runtime / "include", True,
                       lean_object)
        debug_symbols, debug_bss = object_info(debug_object)
        lean_symbols, lean_bss = object_info(lean_object)

    require(PUBLIC_SYMBOLS <= debug_symbols,
            f"debug object lost public API: {sorted(PUBLIC_SYMBOLS - debug_symbols)}")
    require(PUBLIC_SYMBOLS <= lean_symbols,
            f"lean object lost public API: {sorted(PUBLIC_SYMBOLS - lean_symbols)}")
    storage_symbols = {"g_fntrace_ring", "g_fntrace_seq"}
    require(storage_symbols <= debug_symbols,
            "debug object lost fntrace storage symbols")
    require(not storage_symbols & lean_symbols,
            "lean object still exports raw fntrace storage")
    saved = debug_bss - lean_bss
    require(saved >= 140 * MIB,
            f"lean object saved only {saved} B of uninitialized storage")

    print(f"PASS: fntrace lean contract (-{saved} B uninitialized storage)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
