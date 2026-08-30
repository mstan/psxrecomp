#!/usr/bin/env python3
"""Compile the overlay callback shim against the public runtime headers."""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
INCLUDE = ROOT / "runtime" / "include"


def find_gcc() -> str:
    candidates = [
        os.environ.get("CC"),
        r"C:\msys64\mingw64\bin\gcc.exe" if os.name == "nt" else None,
        shutil.which("gcc"),
        shutil.which("cc"),
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return candidate
    raise SystemExit("gcc/cc is required for the overlay shim compile test")


def main() -> int:
    source = r'''#include "cpu_state.h"
#include "overlay_dispatch_preamble.c.inc"

void func_80010000(CPUState *cpu) {
    psx_cyc_bb_defer_begin();
    psx_cyc_charge(1u);
    psx_advance_cycles(1u);
    cpu->gpr[2] = psx_cyc_load_word(cpu, cpu->gpr[4], 2u, 0u);
    cpu->gpr[3] = psx_cyc_load_half(cpu, cpu->gpr[5], 3u, 0u);
    psx_cyc_bb_defer_flush();
    psx_cyc_bb_defer_end();
    if (psx_slice_block(cpu, 0x80010000u, 1u, 0)) return;
    debug_server_log_call_entry(0x80010000u);
    PGXP_ALU(0u, cpu->gpr[2], cpu->gpr[3], cpu->gpr[4]);
    PGXP_MULDIV(0u, cpu->hi, cpu->lo, cpu->gpr[3], cpu->gpr[4]);
}
'''
    gcc = find_gcc()
    with tempfile.TemporaryDirectory() as temp_dir:
        temp = pathlib.Path(temp_dir)
        c_path = temp / "overlay_shim_contract.c"
        out_path = temp / ("overlay_shim_contract.dll" if os.name == "nt"
                           else "overlay_shim_contract.so")
        c_path.write_text(source, encoding="ascii")
        command = [
            gcc, "-shared", "-O2",
            "-DPSX_OVERLAY_DLL_BUILD",
            "-DPSX_NO_DEBUG_TOOLS",
            "-DPSX_ENABLE_BLOCK_CYCLES=1",
            "-DPSX_OVERLAY_FLAVOR=2",
            "-DPSX_PGXP=1",
        ]
        if os.name != "nt":
            command.append("-fPIC")
        command.extend([
            str(c_path), "-o", str(out_path), f"-I{INCLUDE}", "-lm",
        ])
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            return result.returncode
        if not out_path.is_file():
            raise AssertionError("overlay compiler reported success without output")
    print("PASS: overlay callback shim compiles against runtime headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
