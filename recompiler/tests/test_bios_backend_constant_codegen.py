#!/usr/bin/env python3
"""Verify BIOS backend counts are emitted as C integer constant expressions."""

import argparse
import os
import re
import subprocess
import sys
import tempfile


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    default = os.path.join(root, "recompiler", "build", "psxrecomp-bios.exe")

    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=default)
    args = parser.parse_args()

    if not os.path.isfile(args.recompiler):
        print(f"FAIL: recompiler not found: {args.recompiler}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as out_dir:
        result = subprocess.run(
            [
                args.recompiler,
                "--config",
                os.path.join(root, "bios", "OpenBIOS.toml"),
                "--out-dir",
                out_dir,
            ],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            print(result.stderr or result.stdout, file=sys.stderr)
            return 1

        dispatch_path = os.path.join(out_dir, "OpenBIOS_dispatch.c")
        with open(dispatch_path, encoding="utf-8") as stream:
            generated = stream.read()

    constant = re.compile(
        r"enum \{ OpenBIOS_psx_bios_kernel_body_count = \d+u \};"
    )
    if not constant.search(generated):
        print("FAIL: generated dispatch has no enum backend count", file=sys.stderr)
        return 1
    if "static const uint32_t OpenBIOS_psx_bios_kernel_body_count" in generated:
        print("FAIL: backend count is still emitted as a const object", file=sys.stderr)
        return 1

    print("PASS: BIOS backend count is a C integer constant expression.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
