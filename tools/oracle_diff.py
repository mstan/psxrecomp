#!/usr/bin/env python3
"""
oracle_diff.py -- Golden-oracle RAM comparison tool.

Connects to both the psxrecomp runtime (port 4370) and DuckStation oracle
(port 4371) and compares RAM byte-by-byte.

Usage:
    python tools/oracle_diff.py                     # Full 2MB diff
    python tools/oracle_diff.py 0x0000 0x8000       # Kernel RAM only
    python tools/oracle_diff.py 0x7400 0x7600 -v    # Card state, verbose
"""

import socket
import json
import sys
import time
import argparse


def send_cmd(port, cmd_json, timeout=3.0):
    """Send a JSON command to the debug server and return the parsed response."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(("127.0.0.1", port))
        sock.sendall((json.dumps(cmd_json) + "\n").encode())
        data = b""
        while True:
            chunk = sock.recv(8192)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        return json.loads(data.decode().strip())
    finally:
        sock.close()


def read_ram(port, addr, length):
    """Read RAM from a debug server, returns bytes."""
    resp = send_cmd(port, {"cmd": "read_ram", "addr": f"0x{addr:08X}", "len": length})
    if not resp.get("ok"):
        raise RuntimeError(f"read_ram failed on port {port}: {resp}")
    return bytes.fromhex(resp["hex"])


def find_first_divergence(recomp_port, oracle_port, lo=0, hi=0x1FFFFF, context=16, chunk_size=4096):
    """Compare RAM between recomp and oracle, return first difference."""
    first_diff = None
    diff_count = 0
    total_scanned = 0

    addr = lo
    while addr <= hi:
        length = min(chunk_size, hi - addr + 1)
        recomp_data = read_ram(recomp_port, addr, length)
        oracle_data = read_ram(oracle_port, addr, length)

        for i in range(len(recomp_data)):
            total_scanned += 1
            if recomp_data[i] != oracle_data[i]:
                diff_count += 1
                if first_diff is None:
                    first_diff = addr + i

        addr += length

    if first_diff is None:
        return {
            "match": True,
            "bytes_scanned": total_scanned,
        }

    # Read context around first difference
    ctx_lo = max(lo, first_diff - context)
    ctx_hi = min(hi, first_diff + context)
    ctx_len = ctx_hi - ctx_lo + 1
    recomp_ctx = read_ram(recomp_port, ctx_lo, ctx_len)
    oracle_ctx = read_ram(oracle_port, ctx_lo, ctx_len)

    return {
        "match": False,
        "first_diff": first_diff,
        "recomp_val": recomp_ctx[first_diff - ctx_lo],
        "oracle_val": oracle_ctx[first_diff - ctx_lo],
        "diff_count": diff_count,
        "bytes_scanned": total_scanned,
        "ctx_lo": ctx_lo,
        "ctx_hi": ctx_hi,
        "recomp_ctx": recomp_ctx.hex(),
        "oracle_ctx": oracle_ctx.hex(),
    }


def print_context(result, verbose=False):
    """Pretty-print a divergence result with hex context."""
    if result["match"]:
        print(f"MATCH: {result['bytes_scanned']:,} bytes scanned, no differences.")
        return

    addr = result["first_diff"]
    print(f"DIVERGENCE at 0x{addr:08X}: recomp=0x{result['recomp_val']:02X} oracle=0x{result['oracle_val']:02X}")
    print(f"  Total differences: {result['diff_count']:,} / {result['bytes_scanned']:,} bytes")

    if verbose:
        ctx_lo = result["ctx_lo"]
        recomp = bytes.fromhex(result["recomp_ctx"])
        oracle = bytes.fromhex(result["oracle_ctx"])
        print(f"\n  Context [0x{ctx_lo:08X} .. 0x{result['ctx_hi']:08X}]:")
        print(f"  {'Addr':>10s}  {'Recomp':>6s}  {'Oracle':>6s}  {'Match':>5s}")
        for i in range(len(recomp)):
            a = ctx_lo + i
            r = recomp[i]
            o = oracle[i]
            marker = "  " if r == o else " <---"
            print(f"  0x{a:08X}    0x{r:02X}    0x{o:02X}  {marker}")


def main():
    parser = argparse.ArgumentParser(description="Oracle RAM comparison tool")
    parser.add_argument("lo", nargs="?", default="0x0000", help="Start address (hex)")
    parser.add_argument("hi", nargs="?", default="0x1FFFFF", help="End address (hex)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show context bytes")
    parser.add_argument("--recomp-port", type=int, default=4370, help="Recomp debug port")
    parser.add_argument("--oracle-port", type=int, default=4371, help="Oracle debug port")
    parser.add_argument("--context", type=int, default=16, help="Context bytes around first diff")
    args = parser.parse_args()

    lo = int(args.lo, 0)
    hi = int(args.hi, 0)

    # Verify both servers are reachable
    for port, name in [(args.recomp_port, "recomp"), (args.oracle_port, "oracle")]:
        try:
            resp = send_cmd(port, {"cmd": "ping"})
            if resp.get("ok"):
                print(f"  {name} (port {port}): connected (frame {resp.get('frame', '?')})")
            else:
                print(f"  {name} (port {port}): ping failed")
                sys.exit(1)
        except Exception as e:
            print(f"  {name} (port {port}): connection failed: {e}")
            sys.exit(1)

    print(f"\nComparing RAM 0x{lo:08X}..0x{hi:08X} ({hi - lo + 1:,} bytes)...")
    result = find_first_divergence(args.recomp_port, args.oracle_port, lo, hi, args.context)
    print()
    print_context(result, args.verbose)


if __name__ == "__main__":
    main()
