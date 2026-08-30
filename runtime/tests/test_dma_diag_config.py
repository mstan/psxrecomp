#!/usr/bin/env python3
"""Verify lean DMA diagnostics retain their API without storage or producers."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from test_cd_spu_trace_config import (
    is_msvc,
    object_info,
    require,
    simple_function_body,
)

MIB = 1024 * 1024
PUBLIC_SYMBOLS = {
    "dma_debug_get_trace",
    "dma_debug_clear_trace",
    "dma_debug_get_cdrom_history",
    "dma_debug_clear_cdrom_history",
}
STORAGE_SYMBOLS = {
    "dma_trace",
    "dma_trace_seq",
    "cdrom_dma_history",
    "cdrom_dma_history_seq",
    "cdrom_dma_active_entry",
    "cdrom_dma_history_active",
}
PRODUCER_SYMBOLS = {
    "trace_dma",
    "trace_dma_reg_write",
    "start_cdrom_dma_capture",
    "record_cdrom_dma_word",
    "finish_cdrom_dma_capture",
}


def compile_and_preprocess(compiler: Path, source: Path, include_dir: Path,
                           lean: bool, output: Path) -> str:
    if is_msvc(compiler):
        define = ["/DPSX_NO_DEBUG_TOOLS=1"] if lean else []
        common = [str(compiler), "/nologo", "/std:c11", "/Od", *define,
                  f"/I{include_dir}", f"/I{source.parent}"]
        subprocess.run([*common, "/c", str(source), f"/Fo{output}"], check=True)
        result = subprocess.run(
            [*common, "/EP", str(source)], check=True,
            stdout=subprocess.PIPE, text=True, encoding="utf-8",
            errors="replace")
    else:
        define = ["-DPSX_NO_DEBUG_TOOLS=1"] if lean else []
        common = [str(compiler), "-std=c99", "-O0", *define,
                  f"-I{include_dir}", f"-I{source.parent}"]
        subprocess.run([*common, "-c", str(source), "-o", str(output)],
                       check=True)
        result = subprocess.run(
            [*common, "-E", "-P", str(source)], check=True,
            stdout=subprocess.PIPE, text=True, encoding="utf-8",
            errors="replace")
    return " ".join(result.stdout.split())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()

    runtime = args.runtime.resolve()
    source = runtime / "src/dma.c"
    source_text = source.read_text(encoding="utf-8")
    require("#ifndef PSX_NO_DEBUG_TOOLS\nstatic DMATraceEntry dma_trace[" in
            source_text, "DMA trace storage is not debug-only")
    for macro in PRODUCER_SYMBOLS:
        require(f"#define {macro}(" in source_text,
                f"lean producer stub is missing: {macro}")

    with tempfile.TemporaryDirectory(prefix="dma-diag-config-") as temp_name:
        temp = Path(temp_name)
        suffix = ".obj" if is_msvc(args.compiler) else ".o"
        debug_object = temp / f"dma-debug{suffix}"
        lean_object = temp / f"dma-lean{suffix}"
        debug_source = compile_and_preprocess(
            args.compiler, source, runtime / "include", False, debug_object)
        lean_source = compile_and_preprocess(
            args.compiler, source, runtime / "include", True, lean_object)
        debug_symbols, debug_bss = object_info(debug_object)
        lean_symbols, lean_bss = object_info(lean_object)

    require("static DMATraceEntry dma_trace[" in debug_source,
            "debug DMA trace storage was compiled out")
    require("static DMATraceEntry dma_trace[" not in lean_source,
            "lean DMA trace storage survived preprocessing")
    require(PUBLIC_SYMBOLS <= debug_symbols,
            f"debug object lost public API: {sorted(PUBLIC_SYMBOLS - debug_symbols)}")
    require(PUBLIC_SYMBOLS <= lean_symbols,
            f"lean object lost public API: {sorted(PUBLIC_SYMBOLS - lean_symbols)}")
    require(STORAGE_SYMBOLS <= debug_symbols,
            f"debug object lost storage: {sorted(STORAGE_SYMBOLS - debug_symbols)}")
    require(not (STORAGE_SYMBOLS | PRODUCER_SYMBOLS) & lean_symbols,
            "lean object retained DMA diagnostic storage or producers")

    for name in ("dma_debug_get_trace", "dma_debug_get_cdrom_history"):
        body = simple_function_body(lean_source, name)
        require("if (out_entries) *out_entries =" in body and
                body.endswith("return 0;"),
                f"{name} is not a zero/NULL lean stub")
    for name in ("dma_debug_clear_trace", "dma_debug_clear_cdrom_history"):
        require(simple_function_body(lean_source, name) == "",
                f"{name} is not an empty lean stub")

    saved = debug_bss - lean_bss
    require(saved >= 2 * MIB,
            f"lean DMA object saved only {saved} B of uninitialized storage")
    print(f"PASS: DMA lean diagnostic contract (-{saved} B uninitialized storage)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
