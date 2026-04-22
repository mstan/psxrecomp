#!/usr/bin/env python3
"""
debug_client.py — Unified TCP debug client for PSX recomp + DuckStation

Usage:
    python debug_client.py [options] <command> [args...]
    python debug_client.py                          # interactive REPL (native)
    python debug_client.py --ds                     # interactive REPL (DuckStation)

Options:
    --port PORT     Target port (default: 4370 for native)
    --ds            Shorthand for --port 4371 (DuckStation)
    --host HOST     Target host (default: 127.0.0.1)

Commands (shared — identical on both servers):
    ping                        Heartbeat + current frame
    frame                       Current frame number
    regs / get_registers        Dump all MIPS registers
    read <addr> [len]           Read RAM bytes (hex addr, default 16 bytes)
    dump <addr> [len]           Chunked RAM read (up to 64KB)
    write <addr> <hex>          Write RAM bytes
    scratch <addr> [len]        Read scratchpad bytes
    gpu                         GPU display state
    overlay                     Overlay state
    watch <addr>                Set byte watchpoint
    unwatch <addr>              Remove watchpoint
    pause                       Pause execution
    continue / c                Resume execution
    step [n]                    Step N frames (default 1)
    run_to <frame>              Run to specific frame, then pause
    history                     Ring buffer stats
    get_frame <n>               Get full frame record from ring buffer
    range <start> <end>         Frame range query (max 200)
    ts <start> <end>            Frame timeseries — compact (max 200)
    set_snapshot <slot> <addr>  Configure RAM snapshot region (slot 0-3)
    get_snapshots               Show snapshot region config
    input <buttons_hex>         Override controller input
    clear_input                 Remove input override
    quit                        Quit game

Tier 1 write trace commands:
    wtrace_add <lo> <hi>        Add a trace range (up to 8, hex addrs)
    wtrace_del <slot>           Remove trace range by slot index
    wtrace_ranges               List active trace ranges
    wtrace [lo] [hi]            Dump write trace (optional addr filter)
    wtrace_clear                Reset write trace ring buffer
    wtrace_stats                Write trace statistics
    mmio [addr]                 Dump MMIO write trace (optional addr filter)
    mmio_clear                  Reset MMIO trace ring buffer

Comparison commands (query both servers):
    compare <command> [args...] Run command on both ports, diff results
    ts_compare <start> <end>    Compare timeseries across servers

REPL commands:
    target native               Switch to port 4370
    target ds                   Switch to port 4371
    help                        Show this help
"""

import json
import socket
import sys
import argparse

DEFAULT_HOST = "127.0.0.1"
NATIVE_PORT = 4370
DS_PORT = 4371

REG_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]


# ---------------------------------------------------------------------------
# Low-level transport
# ---------------------------------------------------------------------------

def connect(host=DEFAULT_HOST, port=NATIVE_PORT, timeout=10.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    return s


def send_cmd(sock, cmd_dict):
    line = json.dumps(cmd_dict) + "\n"
    sock.sendall(line.encode())
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
    return json.loads(buf.decode().strip())


def query(host, port, cmd_dict):
    """One-shot: connect, send, receive, close."""
    s = connect(host, port)
    try:
        return send_cmd(s, cmd_dict)
    finally:
        s.close()


# ---------------------------------------------------------------------------
# Formatters
# ---------------------------------------------------------------------------

def pretty_regs(resp):
    if not resp.get("ok"):
        return json.dumps(resp, indent=2)
    lines = [f"  PC: {resp.get('pc','?')}  HI: {resp.get('hi','?')}  LO: {resp.get('lo','?')}"]
    regs = resp.get("regs", {})
    for i in range(0, 32, 4):
        parts = []
        for j in range(4):
            name = REG_NAMES[i + j]
            val = regs.get(name, "?")
            parts.append(f"{name:>4s}: {val}")
        lines.append("  " + "  ".join(parts))
    return "\n".join(lines)


def pretty_json(resp):
    return json.dumps(resp, indent=2)


# ---------------------------------------------------------------------------
# Command builder — translates CLI args to JSON wire format
# ---------------------------------------------------------------------------

def build_cmd(args):
    """Convert CLI args to (wire_cmd_dict, formatter) tuple."""
    if not args:
        return None, None
    cmd = args[0].lower()

    if cmd == "ping":
        return {"cmd": "ping"}, pretty_json
    elif cmd == "frame":
        return {"cmd": "frame"}, pretty_json
    elif cmd in ("regs", "get_registers"):
        return {"cmd": "get_registers"}, pretty_regs
    elif cmd == "read":
        addr = args[1] if len(args) > 1 else "0x80010000"
        length = int(args[2]) if len(args) > 2 else 16
        return {"cmd": "read_ram", "addr": addr, "len": length}, pretty_json
    elif cmd == "dump":
        addr = args[1] if len(args) > 1 else "0x80010000"
        length = int(args[2]) if len(args) > 2 else 256
        return {"cmd": "dump_ram", "addr": addr, "len": length}, pretty_json
    elif cmd == "write":
        if len(args) < 3:
            return None, lambda _: "Usage: write <addr> <hex>"
        return {"cmd": "write_ram", "addr": args[1], "hex": args[2]}, pretty_json
    elif cmd == "scratch":
        addr = args[1] if len(args) > 1 else "0x1F800000"
        length = int(args[2]) if len(args) > 2 else 16
        return {"cmd": "read_scratch", "addr": addr, "len": length}, pretty_json
    elif cmd == "gpu":
        return {"cmd": "gpu_state"}, pretty_json
    elif cmd == "overlay":
        return {"cmd": "overlay_state"}, pretty_json
    elif cmd == "watch":
        if len(args) < 2:
            return None, lambda _: "Usage: watch <addr>"
        return {"cmd": "watch", "addr": args[1]}, pretty_json
    elif cmd == "unwatch":
        if len(args) < 2:
            return None, lambda _: "Usage: unwatch <addr>"
        return {"cmd": "unwatch", "addr": args[1]}, pretty_json
    elif cmd == "pause":
        return {"cmd": "pause"}, pretty_json
    elif cmd in ("continue", "c"):
        return {"cmd": "continue"}, pretty_json
    elif cmd == "step":
        n = int(args[1]) if len(args) > 1 else 1
        return {"cmd": "step", "count": n}, pretty_json
    elif cmd == "run_to":
        if len(args) < 2:
            return None, lambda _: "Usage: run_to <frame>"
        return {"cmd": "run_to_frame", "frame": int(args[1])}, pretty_json
    elif cmd == "history":
        return {"cmd": "history"}, pretty_json
    elif cmd == "get_frame":
        if len(args) < 2:
            return None, lambda _: "Usage: get_frame <n>"
        return {"cmd": "get_frame", "frame": int(args[1])}, pretty_json
    elif cmd == "range":
        if len(args) < 3:
            return None, lambda _: "Usage: range <start> <end>"
        return {"cmd": "frame_range", "start": int(args[1]), "end": int(args[2])}, pretty_json
    elif cmd == "ts":
        if len(args) < 3:
            return None, lambda _: "Usage: ts <start> <end>"
        return {"cmd": "frame_timeseries", "start": int(args[1]), "end": int(args[2])}, pretty_json
    elif cmd == "set_snapshot":
        if len(args) < 3:
            return None, lambda _: "Usage: set_snapshot <slot> <addr>"
        return {"cmd": "set_snapshot", "slot": int(args[1]), "addr": args[2]}, pretty_json
    elif cmd == "get_snapshots":
        return {"cmd": "get_snapshots"}, pretty_json
    elif cmd == "input":
        if len(args) < 2:
            return None, lambda _: "Usage: input <buttons_hex>"
        return {"cmd": "set_input", "buttons": args[1]}, pretty_json
    elif cmd == "clear_input":
        return {"cmd": "clear_input"}, pretty_json
    elif cmd == "quit":
        return {"cmd": "quit"}, pretty_json
    elif cmd == "dispatch_check":
        if len(args) < 2:
            return None, lambda _: "Usage: dispatch_check <addr>"
        return {"cmd": "dispatch_check", "addr": args[1]}, pretty_json
    elif cmd == "dispatch_tail":
        count = int(args[1]) if len(args) > 1 else 64
        return {"cmd": "dispatch_tail", "count": str(count)}, pretty_json
    elif cmd == "screenshot":
        d = {"cmd": "screenshot"}
        if len(args) > 1:
            d["path"] = args[1]
        return d, pretty_json
    elif cmd == "bios_trace":
        d = {"cmd": "bios_trace"}
        if len(args) > 1:
            d["limit"] = int(args[1])
        return d, pretty_json
    elif cmd == "sio_state":
        return {"cmd": "sio_state"}, pretty_json
    elif cmd == "bios_wait_state":
        return {"cmd": "bios_wait_state"}, pretty_json
    elif cmd == "memcard_log":
        return {"cmd": "memcard_log"}, pretty_json
    # ---- Tier 1 write trace commands ----
    elif cmd == "wtrace_add":
        if len(args) < 3:
            return None, lambda _: "Usage: wtrace_add <lo> <hi>"
        return {"cmd": "wtrace_add", "lo": args[1], "hi": args[2]}, pretty_json
    elif cmd == "wtrace_del":
        if len(args) < 2:
            return None, lambda _: "Usage: wtrace_del <slot>"
        return {"cmd": "wtrace_del", "slot": int(args[1])}, pretty_json
    elif cmd == "wtrace_ranges":
        return {"cmd": "wtrace_ranges"}, pretty_json
    elif cmd == "wtrace":
        d = {"cmd": "wtrace_dump"}
        if len(args) > 1:
            d["addr_lo"] = args[1]
        if len(args) > 2:
            d["addr_hi"] = args[2]
        return d, pretty_json
    elif cmd == "wtrace_clear":
        return {"cmd": "wtrace_clear"}, pretty_json
    elif cmd == "wtrace_stats":
        return {"cmd": "wtrace_stats"}, pretty_json
    elif cmd == "mmio":
        d = {"cmd": "mmio_dump"}
        if len(args) > 1:
            d["addr"] = args[1]
        return d, pretty_json
    elif cmd == "mmio_clear":
        return {"cmd": "mmio_clear"}, pretty_json
    else:
        # Pass through as raw command
        return {"cmd": cmd}, pretty_json


# ---------------------------------------------------------------------------
# Comparison utilities
# ---------------------------------------------------------------------------

def diff_json(label_a, resp_a, label_b, resp_b):
    """Compare two JSON responses and print differences."""
    lines = []

    # Strip id field for comparison
    a = {k: v for k, v in resp_a.items() if k != "id"}
    b = {k: v for k, v in resp_b.items() if k != "id"}

    all_keys = sorted(set(list(a.keys()) + list(b.keys())))
    diffs = 0
    for key in all_keys:
        va = a.get(key)
        vb = b.get(key)
        if va == vb:
            lines.append(f"  {key}: {va}")
        else:
            lines.append(f"  {key}:")
            lines.append(f"    {label_a}: {va}")
            lines.append(f"    {label_b}: {vb}")
            diffs += 1

    header = f"=== Compare: {diffs} difference(s) ==="
    return header + "\n" + "\n".join(lines)


def run_compare(host, args):
    """Run the same command on both native and DS, diff results."""
    cmd_dict, fmt = build_cmd(args)
    if cmd_dict is None:
        return fmt(None) if fmt else "No command"

    try:
        resp_native = query(host, NATIVE_PORT, cmd_dict)
    except (ConnectionRefusedError, TimeoutError, OSError) as e:
        resp_native = {"error": f"Cannot connect to native (port {NATIVE_PORT}): {e}"}

    try:
        resp_ds = query(host, DS_PORT, cmd_dict)
    except (ConnectionRefusedError, TimeoutError, OSError) as e:
        resp_ds = {"error": f"Cannot connect to DuckStation (port {DS_PORT}): {e}"}

    return diff_json(f"native:{NATIVE_PORT}", resp_native,
                     f"ds:{DS_PORT}", resp_ds)


def run_ts_compare(host, start, end):
    """Fetch timeseries from both servers, align by frame, highlight divergences."""
    cmd = {"cmd": "frame_timeseries", "start": start, "end": end}
    try:
        native = query(host, NATIVE_PORT, cmd)
    except (ConnectionRefusedError, TimeoutError, OSError):
        return f"Cannot connect to native (port {NATIVE_PORT})"
    try:
        ds = query(host, DS_PORT, cmd)
    except (ConnectionRefusedError, TimeoutError, OSError):
        return f"Cannot connect to DuckStation (port {DS_PORT})"

    ts_n = native.get("ts", [])
    ts_d = ds.get("ts", [])

    lines = [f"=== Timeseries Compare: frames {start}-{end} ==="]
    lines.append(f"{'Frame':>8} {'Field':>8} {'Native':>12} {'DS':>12}")
    lines.append("-" * 48)

    max_len = max(len(ts_n), len(ts_d))
    diffs = 0
    for i in range(max_len):
        fn = ts_n[i] if i < len(ts_n) else None
        fd = ts_d[i] if i < len(ts_d) else None
        if fn is None and fd is None:
            continue
        if fn is None:
            frame = fd.get("f", start + i) if fd else start + i
            lines.append(f"{frame:>8} {'*':>8} {'(missing)':>12} {'present':>12}")
            diffs += 1
            continue
        if fd is None:
            frame = fn.get("f", start + i) if fn else start + i
            lines.append(f"{frame:>8} {'*':>8} {'present':>12} {'(missing)':>12}")
            diffs += 1
            continue

        frame = fn.get("f", start + i)
        # Compare shared fields
        for key in ("pc", "sp", "ra", "pad", "da_x", "da_y"):
            vn = fn.get(key)
            vd = fd.get(key)
            if vn != vd:
                lines.append(f"{frame:>8} {key:>8} {str(vn):>12} {str(vd):>12}")
                diffs += 1

    lines.append(f"\nTotal differences: {diffs}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Command execution
# ---------------------------------------------------------------------------

def run_command(sock, args, host=DEFAULT_HOST):
    """Execute a command on the given socket. Returns formatted string."""
    if not args:
        return None

    cmd = args[0].lower()

    # Meta-commands
    if cmd == "compare":
        return run_compare(host, args[1:])
    if cmd == "ts_compare":
        if len(args) < 3:
            return "Usage: ts_compare <start> <end>"
        return run_ts_compare(host, int(args[1]), int(args[2]))

    cmd_dict, fmt = build_cmd(args)
    if cmd_dict is None:
        return fmt(None) if fmt else None

    resp = send_cmd(sock, cmd_dict)
    return fmt(resp)


# ---------------------------------------------------------------------------
# REPL
# ---------------------------------------------------------------------------

def repl(host, port):
    try:
        sock = connect(host, port)
    except ConnectionRefusedError:
        print(f"Cannot connect to {host}:{port} -- is the target running?")
        sys.exit(1)

    target = "native" if port == NATIVE_PORT else "ds" if port == DS_PORT else f":{port}"
    print(f"Connected to {host}:{port} ({target})")
    print("Type 'help' for commands, Ctrl-C to exit\n")

    try:
        while True:
            try:
                line = input(f"psx({target})> ").strip()
            except EOFError:
                break
            if not line:
                continue
            if line.lower() == "help":
                print(__doc__)
                continue

            args = line.split()

            # Target switching
            if args[0].lower() == "target":
                if len(args) < 2:
                    print(f"Current target: {target} ({host}:{port})")
                    continue
                new_target = args[1].lower()
                new_port = NATIVE_PORT if new_target == "native" else DS_PORT if new_target == "ds" else int(new_target)
                try:
                    sock.close()
                except Exception:
                    pass
                try:
                    sock = connect(host, new_port)
                    port = new_port
                    target = new_target
                    print(f"Switched to {host}:{port} ({target})")
                except ConnectionRefusedError:
                    print(f"Cannot connect to {host}:{new_port}")
                continue

            try:
                result = run_command(sock, args, host)
                if result:
                    print(result)
            except (ConnectionResetError, BrokenPipeError, socket.timeout):
                print("Connection lost. Reconnecting...")
                try:
                    sock = connect(host, port)
                    print("Reconnected.")
                except ConnectionRefusedError:
                    print(f"Cannot reconnect to {host}:{port}")
                    break
    except KeyboardInterrupt:
        print()
    finally:
        sock.close()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Unified TCP debug client for PSX recomp + DuckStation",
        add_help=False,
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--ds", action="store_true", help="Target DuckStation (port 4371)")
    parser.add_argument("args", nargs="*")

    opts = parser.parse_args()

    port = opts.port
    if port is None:
        port = DS_PORT if opts.ds else NATIVE_PORT

    if not opts.args:
        # REPL mode
        repl(opts.host, port)
        return

    # Single command mode
    try:
        sock = connect(opts.host, port)
    except ConnectionRefusedError:
        print(f"Cannot connect to {opts.host}:{port} -- is the target running?")
        sys.exit(1)

    try:
        result = run_command(sock, opts.args, opts.host)
        if result:
            print(result)
    finally:
        sock.close()


if __name__ == "__main__":
    main()
