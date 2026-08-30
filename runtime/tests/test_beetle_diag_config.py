#!/usr/bin/env python3
"""Verify psx-beetle's target-local diagnostic rings compile out in lean builds."""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

STATIC_RING_BYTES = (
    8 * 65536                 # SIO
    + 16 * 8192               # CD command
    + 32 * 262144             # all-write trace
    + 72 * 65536              # rich write trace
    + 32 * 65536              # read trace
    + 48 * 65536              # function trace
    + 32 * (1 << 20)          # SPU events
)
HISTORY_BYTES = 728 * 36000


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def preprocess(compiler: Path, source: Path, lean: bool, temp: Path) -> str:
    # The Beetle SDK is optional and is not present in ordinary runtime-only
    # test checkouts. Preprocessing does not need its declarations, so remove
    # includes while retaining the production file's exact conditional graph.
    sanitized = temp / ("beetle-lean.cpp" if lean else "beetle-debug.cpp")
    text = source.read_text(encoding="utf-8")
    text = re.sub(r"^\s*#\s*include\b.*$", "", text, flags=re.MULTILINE)
    sanitized.write_text(text, encoding="utf-8")
    command = [str(compiler), "-E", "-P", "-x", "c++"]
    if lean:
        command.append("-DPSX_NO_DEBUG_TOOLS=1")
    result = subprocess.run(
        [*command, str(sanitized)], check=True, stdout=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace")
    return " ".join(result.stdout.split())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()
    source = args.runtime.resolve() / "src/beetle_libretro.cpp"

    with tempfile.TemporaryDirectory(prefix="beetle-diag-") as temp_name:
        temp = Path(temp_name)
        debug = preprocess(args.compiler, source, False, temp)
        lean = preprocess(args.compiler, source, True, temp)

    storage = (
        "s_sio_trace[65536]", "s_cdcmd_trace[8192]",
        "s_wtrace_all[262144]", "s_wtrace[65536]", "s_rtrace[65536]",
        "s_fntrace[65536]", "s_spu_events[(1u << 20)]",
    )
    for declaration in storage:
        require(declaration in debug, f"debug storage missing: {declaration}")
        require(declaration not in lean, f"lean storage survived: {declaration}")

    for producer in ("sio_trace_callback", "cdcmd_trace_callback",
                     "wtrace_callback", "rtrace_callback", "fntrace_callback"):
        require(producer in debug, f"debug producer missing: {producer}")
        require(producer not in lean, f"lean producer survived: {producer}")
    require("SetSIOTraceCallback" in debug and "SetSIOTraceCallback" not in lean,
            "lean build still registers the SIO producer")
    history_allocation = "sizeof(BeetleFrameRecord)"
    require("std::calloc" in debug and history_allocation in debug,
            "debug history allocation missing")
    require("std::calloc" not in lean and history_allocation not in lean,
            "lean history allocation survived")

    public_exports = (
        "beetle_get_sio_trace", "beetle_get_cdcmd_trace",
        "beetle_wtrace_arm", "beetle_wtrace_get_rich",
        "beetle_rtrace_arm", "beetle_rtrace_get",
        "beetle_wtrace_all_get", "beetle_fntrace_arm",
        "beetle_fntrace_get", "psxrecomp_beetle_spu_event",
        "beetle_spu_event_get", "beetle_history_get_bounds",
        "beetle_history_get_frame",
    )
    for symbol in public_exports:
        require(symbol in lean, f"lean build lost public export: {symbol}")

    print("PASS: Beetle lean diagnostics contract "
          f"(static -{STATIC_RING_BYTES} B, startup heap -{HISTORY_BYTES} B)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
