#!/usr/bin/env python3
"""Regression for an overlay branch whose delay slot crosses a 4 KiB page.

The complete input must emit and hash the delay slot.  A page-truncated input
must fail code generation instead of publishing a native transfer that skips it.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from typing import Optional


LOAD = 0x80010FF0
BRANCH_PC = 0x80010FFC
DELAY_PC = 0x80011000


def make_psxexe(data: bytes) -> bytes:
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)
    struct.pack_into("<I", header, 0x18, LOAD)
    struct.pack_into("<I", header, 0x1C, len(data))
    return bytes(header) + data


def run_codegen(recompiler: str, include_delay: bool, root: str,
                branch_word: int = 0x10800008,
                case_name: Optional[str] = None):
    case = case_name or ("complete" if include_delay else "truncated")
    case_dir = os.path.join(root, case)
    out_dir = os.path.join(case_dir, "out")
    os.makedirs(out_dir)
    # FF0..FF8: nops; FFC: beq $a0,$zero,0x80011020; 1000: addiu $v0,$zero,1.
    words = [0, 0, 0, branch_word]
    if include_delay:
        words.append(0x24020001)
    data = b"".join(struct.pack("<I", word) for word in words)
    psx = os.path.join(case_dir, "cross_page.psx")
    seeds = os.path.join(case_dir, "seeds.txt")
    with open(psx, "wb") as f:
        f.write(make_psxexe(data))
    with open(seeds, "w", encoding="ascii") as f:
        f.write(f"dispatch_root 0x{LOAD:08X}\n")
    proc = subprocess.run(
        [recompiler, psx, "--seeds", seeds, "--out-dir", out_dir, "--overlay"],
        capture_output=True,
        text=True,
    )
    return proc, out_dir


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_recompiler = os.path.normpath(
        os.path.join(here, "..", "build", "psxrecomp-game.exe")
    )
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=default_recompiler)
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    failures = []
    with tempfile.TemporaryDirectory() as root:
        complete, complete_out = run_codegen(args.recompiler, True, root)
        if complete.returncode != 0:
            failures.append("complete cross-page function failed generation: " +
                            (complete.stderr or complete.stdout))
        else:
            full_c = next((name for name in os.listdir(complete_out)
                           if name.endswith("_full.c")), None)
            ranges = next((name for name in os.listdir(complete_out)
                           if name.endswith("_full.ranges")), None)
            if not full_c or not ranges:
                failures.append("complete generation omitted C or ranges output")
            else:
                with open(os.path.join(complete_out, full_c), encoding="utf-8") as f:
                    source = f.read()
                with open(os.path.join(complete_out, ranges), encoding="utf-8") as f:
                    manifest = f.read()
                if "0x80011000" not in source or "0x24020001" not in source:
                    failures.append("generated C does not execute the cross-page delay slot")

                covered = False
                for line in manifest.splitlines():
                    fields = line.split()
                    if len(fields) == 3 and fields[0] == "R":
                        lo = int(fields[1], 16)
                        length = int(fields[2], 16)
                        if lo <= DELAY_PC and DELAY_PC + 4 <= lo + length:
                            covered = True
                if not covered:
                    failures.append("range manifest does not hash the cross-page delay slot")

        truncated, _ = run_codegen(args.recompiler, False, root)
        combined = (truncated.stderr or "") + (truncated.stdout or "")
        if truncated.returncode == 0:
            failures.append("truncated input incorrectly generated a native shard")
        elif "mandatory delay slot" not in combined:
            failures.append("truncated generation failed without the delay-slot diagnostic")

        likely, likely_out = run_codegen(
            args.recompiler, True, root, branch_word=0x50800008,
            case_name="reserved_branch_likely")
        likely_output = (likely.stderr or "") + (likely.stdout or "")
        if likely.returncode != 0:
            failures.append("reserved R3000A opcode aborted generation: " + likely_output)
        else:
            likely_c = next((name for name in os.listdir(likely_out)
                             if name.endswith("_full.c")), None)
            if not likely_c:
                failures.append("reserved R3000A opcode generation omitted C output")
            else:
                with open(os.path.join(likely_out, likely_c), encoding="utf-8") as f:
                    likely_source = f.read()
                ri_markers = (
                    "reserved opcode 0x50800008: architectural RI exception",
                    "cpu->cop0[14] = 0x80010FFCu",
                    "(10u << 2)",
                    "cpu->pc = (psx_ri_sr & 0x00400000u)",
                    "return;",
                )
                missing = [marker for marker in ri_markers if marker not in likely_source]
                if missing:
                    failures.append("reserved R3000A opcode omitted RI raise markers: " +
                                    ", ".join(missing))
            if "emitting guest RI exception raise" not in likely_output:
                failures.append("reserved R3000A opcode omitted its RI diagnostic")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: cross-page delay slot is emitted and hashed, truncation fails closed, "
          "and reserved opcodes emit RI")
    return 0


if __name__ == "__main__":
    sys.exit(main())
