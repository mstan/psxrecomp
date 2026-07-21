#!/usr/bin/env python3
"""Integration regression for overlay store-cycle publication ordering.

Build a tiny PS-X EXE containing every CPU store form emitted by the game
recompiler, generate an overlay shard, and verify that generated host memory
side effects cannot run while instruction cycles are still DLL-local.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


LOAD = 0x80010000


def i_type(op, rs, rt, imm=0):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def make_psxexe(words):
    data = b"".join(struct.pack("<I", word) for word in words)
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)
    struct.pack_into("<I", header, 0x18, LOAD)
    struct.pack_into("<I", header, 0x1C, len(data))
    return bytes(header) + data


def generated_overlay_c(recompiler, tmp):
    # v1 is the address, a0 is the source/GTE register.
    words = [
        i_type(0x28, 3, 4, 0),  # sb
        i_type(0x29, 3, 4, 0),  # sh
        i_type(0x2B, 3, 4, 0),  # sw
        i_type(0x2A, 3, 4, 1),  # swl
        i_type(0x2E, 3, 4, 2),  # swr
        i_type(0x3A, 3, 4, 0),  # swc2
        0x03E00008,              # jr ra
        0x00000000,              # nop (delay)
    ]
    psx = os.path.join(tmp, "stores.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out)
    with open(psx, "wb") as f:
        f.write(make_psxexe(words))
    with open(seeds, "w") as f:
        f.write(f"0x{LOAD:08X}\n")

    result = subprocess.run(
        [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"recompiler failed:\n{result.stderr or result.stdout}")
    full = [name for name in os.listdir(out) if name.endswith("_full.c")]
    if not full:
        raise SystemExit("recompiler emitted no _full.c overlay source")
    with open(os.path.join(out, full[0]), encoding="utf-8") as f:
        return f.read()


def ordered(text, *needles):
    pos = 0
    for needle in needles:
        pos = text.find(needle, pos)
        if pos < 0:
            return False
        pos += len(needle)
    return True


def function_body(source, name):
    start = source.find(f"void {name}(")
    if start < 0:
        return ""
    end = source.find("\n}", start)
    return source[start:end + 2] if end >= 0 else source[start:]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.normpath(os.path.join(here, "..", ".."))
    default_recompiler = os.path.normpath(os.path.join(
        here, "..", "build", "psxrecomp-game.exe"))
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=default_recompiler)
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    with tempfile.TemporaryDirectory() as tmp:
        source = generated_overlay_c(args.recompiler, tmp)

    # Exercise the real DLL post-pass too.  It must inject the preamble that
    # defines the flush target used by the generated barrier; the actual DLL
    # compile supplies PSX_OVERLAY_DLL_BUILD so cpu_state.h selects that path.
    sys.path.insert(0, os.path.join(repo, "tools"))
    import compile_overlays
    compile_overlays.DISPATCH_PREAMBLE = None
    compile_overlays.load_dispatch_preamble(os.path.join(repo, "runtime", "include"))
    source = compile_overlays.patch_generated_c(source, LOAD, 8 * 4)

    failures = []
    flush_definition = source.find("void overlay_flush_cycles(void)")
    first_barrier = source.find("psx_store_cycle_barrier();")
    if flush_definition < 0 or first_barrier < 0 or flush_definition > first_barrier:
        failures.append("DLL post-pass did not inject overlay_flush_cycles before generated code")
    for writer in ("write_byte", "write_half", "write_word"):
        if f"psx_store_cycle_barrier(); cpu->{writer}(" not in source:
            failures.append(f"generated {writer} has no preceding cycle barrier")

    for helper in ("psx_swl", "psx_swr"):
        body = function_body(source, helper)
        if not ordered(body, "psx_store_cycle_barrier();", "cpu->read_word(",
                       "cpu->write_word("):
            failures.append(
                f"{helper} does not publish cycles before its read-modify-write")

    # The SWC2-specific precision shadow must remain after the real write, with
    # the GTE completion stall and cycle publication ahead of both.
    if not ordered(source, "psx_gte_stall(cpu);", "psx_store_cycle_barrier();",
                   "cpu->write_word(", "gte_precision_store_word("):
        failures.append("SWC2 stall/barrier/write/precision ordering is wrong")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: all generated overlay stores publish cycles before host side effects.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
