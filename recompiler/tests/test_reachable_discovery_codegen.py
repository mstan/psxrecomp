#!/usr/bin/env python3
"""End-to-end config and codegen checks for reachable main-EXE discovery.

Usage: python test_reachable_discovery_codegen.py \
           [--recompiler <psxrecomp-game.exe>]
"""
import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

LOAD = 0x80010000


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def put32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def make_psxexe():
    size = 0x2000
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    put32(header, 0x10, LOAD)
    put32(header, 0x18, LOAD)
    put32(header, 0x1C, size)
    text = bytearray(size)
    root = [
        jal(LOAD + 0x100), 0,              # reachable direct call
        0x0320F809, 0,                     # unresolved jalr $ra,$t9
        jal(LOAD + 0x1100), 0,             # outside configured bound
        0x03E00008, 0,
    ]
    for i, word in enumerate(root):
        put32(text, i * 4, word)
    for offset in (0x100, 0x200, 0x1100):
        for i, word in enumerate((0x27BDFFF0, 0x03E00008, 0x27BD0010)):
            put32(text, offset + i * 4, word)
    return bytes(header + text)


def write_config(path, discovery="reachable", text_size="0x1000"):
    discovery_line = (f'discovery = "{discovery}"\n'
                      if discovery is not None else "")
    with open(path, "w", encoding="utf-8") as f:
        f.write(f'''[game]
name = "Reachable discovery test"
exe = "test.exe"
load_address = "0x80010000"
entry_pc = "0x80010000"
text_size = "{text_size}"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
{discovery_line}
''')


def run(recompiler, config):
    return subprocess.run(
        [recompiler, "--config", config], capture_output=True, text=True)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_recompiler = os.path.normpath(os.path.join(
        here, "..", "build", "psxrecomp-game.exe"))
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=default_recompiler)
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test.exe")
        seeds = os.path.join(tmp, "seeds.txt")
        config = os.path.join(tmp, "game.toml")
        with open(exe, "wb") as f:
            f.write(make_psxexe())
        with open(seeds, "w", encoding="utf-8") as f:
            f.write("0x80010000\n")

        write_config(config)
        result = run(args.recompiler, config)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            failures.append(f"reachable recompile failed:\n{output}")
        else:
            full_paths = sorted(path for path in
                                pathlib.Path(tmp, "generated").glob("test.exe_full*.c")
                                if "_dispatch" not in path.name)
            dispatch_path = os.path.join(
                tmp, "generated", "test.exe_dispatch.c")
            if not full_paths:
                failures.append("reachable recompile emitted no _full*.c shards")
                full = ""
            else:
                full = "\n".join(path.read_text(encoding="utf-8")
                                   for path in full_paths)
            with open(dispatch_path, encoding="utf-8") as f:
                dispatch = f.read()

            if "void func_80010000" not in full:
                failures.append("main EXE entry was not emitted")
            if "void func_80010100" not in full:
                failures.append("reachable direct-JAL target was not emitted")
            if "void func_80010200" in full:
                failures.append("unseen indirect callback was emitted")
            if "void func_80011100" in full:
                failures.append("target beyond analysis bound was emitted")
            if "phys < 0x00011000u" not in dispatch:
                failures.append("dispatch text range did not use verified bound")
            if "dirty_ram_text_native_ok_ranges(" not in dispatch:
                failures.append("exact static-range dispatch guard was lost")
            if "psx_native_bad_entry(" in full:
                failures.append("reachable main image incorrectly used overlay codegen")
            if "=== Exact-Entry Function Analysis ===" not in output:
                failures.append("reachable config did not select exact-entry analysis")

        # Omitting the key must retain the established whole-image default.
        write_config(config, discovery=None, text_size="0x2000")
        default_mode = run(args.recompiler, config)
        default_output = default_mode.stdout + default_mode.stderr
        if default_mode.returncode != 0 or "=== Function Boundary Detection ===" not in default_output:
            failures.append("omitted discovery key did not preserve whole-image default")

        write_config(config, discovery="invent-functions")
        invalid_mode = run(args.recompiler, config)
        if invalid_mode.returncode == 0 or "must be 'whole-image' or 'reachable'" not in (
                invalid_mode.stdout + invalid_mode.stderr):
            failures.append("invalid discovery mode did not fail closed")

        write_config(config, text_size="0x1800")
        invalid_bound = run(args.recompiler, config)
        if invalid_bound.returncode == 0 or "must be 4 KiB page-aligned" not in (
                invalid_bound.stdout + invalid_bound.stderr):
            failures.append("invalid analysis bound did not fail closed")

        write_config(config, text_size="0x0")
        zero_bound = run(args.recompiler, config)
        if zero_bound.returncode == 0 or "game.text_size must be nonzero" not in (
                zero_bound.stdout + zero_bound.stderr):
            failures.append("zero analysis bound did not fail closed")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: reachable parser, analysis, and exact static codegen behavior")
    return 0


if __name__ == "__main__":
    sys.exit(main())
