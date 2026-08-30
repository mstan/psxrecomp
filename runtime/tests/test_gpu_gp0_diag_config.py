#!/usr/bin/env python3
"""Compile gpu.c and verify the GP0 diagnostic contract in one build mode."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def require(source: str, needle: str, message: str) -> None:
    if needle not in source:
        raise AssertionError(message)


def forbid(source: str, needle: str, message: str) -> None:
    if needle in source:
        raise AssertionError(message)


def run_compiler(compiler: Path, source: Path, include_dir: Path,
                 mode: str, temp_dir: Path) -> str:
    is_msvc = compiler.name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    define = ["/DPSX_NO_DEBUG_TOOLS=1"] if mode == "release" else []
    if is_msvc:
        obj = temp_dir / "gpu.obj"
        preprocessed = temp_dir / "gpu.i"
        common = [str(compiler), "/nologo", "/std:c11", *define,
                  f"/I{include_dir}", f"/I{source.parent}"]
        subprocess.run([*common, "/c", str(source), f"/Fo{obj}"], check=True)
        subprocess.run([*common, "/P", str(source), f"/Fi{preprocessed}"], check=True)
        return preprocessed.read_text(encoding="utf-8", errors="replace")

    define = ["-DPSX_NO_DEBUG_TOOLS=1"] if mode == "release" else []
    obj = temp_dir / "gpu.o"
    common = [str(compiler), "-std=c99", *define,
              f"-I{include_dir}", f"-I{source.parent}"]
    subprocess.run([*common, "-c", str(source), "-o", str(obj)], check=True)
    completed = subprocess.run(
        [*common, "-E", "-P", str(source)], check=True,
        stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("debug", "release"))
    parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()

    source = args.runtime / "src/gpu.c"
    with tempfile.TemporaryDirectory(prefix=f"gp0-{args.mode}-") as temp:
        preprocessed = run_compiler(args.compiler, source,
                                    args.runtime / "include", args.mode,
                                    Path(temp))

    flat = " ".join(preprocessed.split())
    require(flat, "uint64_t gpu_get_gp0_count(void) { return gp0_write_count; }",
            "total GP0 write tracking disappeared")
    require(flat, "gp0_write_count++;",
            "GP0 writes no longer increment the functional total")
    require(flat, "void gpu_set_gp0_source(uint32_t addr) { gp0_next_source_addr = addr; }",
            "GP0 source tracking disappeared")
    require(flat, "uint32_t gpu_gp0_ring_max_words(void) { return 12; }",
            "wire-protocol max_words changed or became configuration-dependent")

    if args.mode == "debug":
        require(flat, "static uint32_t gp0_opcode_count[256];",
                "debug opcode counters were compiled out")
        require(flat, "static GpuGp0RingEntry *gp0_ring",
                "debug GP0 ring storage was compiled out")
        require(flat, "calloc((1u << 20), sizeof(*gp0_ring))",
                "debug GP0 ring allocation was compiled out")
        require(flat, "uint64_t gpu_gp0_ring_total(void) { return gp0_ring_seq; }",
                "debug total accessor no longer reports the ring sequence")
    else:
        forbid(flat, "gp0_opcode_count[256]",
               "release still retains opcode counter storage")
        forbid(flat, "gp0_ring_seq",
               "release still retains the GP0 diagnostic ring state")
        forbid(flat, "gp0_capture_builder_chain",
               "release still retains GP0 stack-unwind diagnostics")
        require(flat, "uint32_t gpu_get_opcode_count(uint8_t op) { (void)op; return 0; }",
                "release opcode accessor is not a zero stub")
        require(flat, "uint64_t gpu_gp0_ring_total(void) { return 0; }",
                "release ring total is not zero")
        require(flat, "uint32_t gpu_gp0_ring_capacity(void) { return 0; }",
                "release ring capacity is not zero")
        require(flat, "(void)max_out; return 0; } void gpu_gp0_ring_frame_span",
                "release frame dump is not a zero-result stub")
        require(flat, "if (out_oldest) *out_oldest = 0; if (out_newest) *out_newest = 0;",
                "release frame span does not zero every non-null output")
        require(flat, "if (nop) *nop = 0; if (fill) *fill = 0; if (draw) *draw = 0; if (env) *env = 0; if (copy) *copy = 0;",
                "release GP0 class counters do not zero every non-null output")

    print(f"PASS: gpu.c GP0 diagnostics contract ({args.mode})")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
