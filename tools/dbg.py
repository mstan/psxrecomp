#!/usr/bin/env python3
"""
dbg.py — TCP debug client for PSX recomp projects

Usage:
    python dbg.py [command] [args...]       # single command mode
    python dbg.py                           # interactive REPL mode

Commands:
    ping                        Heartbeat check
    frame                       Current frame number
    regs / get_registers        Dump all MIPS registers
    read <addr> [len]           Read RAM bytes (hex)
    write <addr> <hex>          Write RAM bytes
    scratch <addr> [len]        Read scratchpad bytes
    gpu                         GPU display state
    overlay                     Overlay state
    watch <addr>                Set byte watchpoint
    unwatch <addr>              Remove watchpoint
    pause                       Pause game
    continue / c                Resume game
    step [n]                    Step N frames (default 1)
    run_to <frame>              Run to specific frame
    history                     Ring buffer stats
    get_frame <n>               Get frame record
    range <start> <end>         Frame range query
    ts <start> <end>            Frame timeseries (compact)
    input <buttons_hex>         Override controller input
    clear_input                 Remove input override
    quit                        Quit game
"""

import json
import socket
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 4370

REG_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]


def connect(host=DEFAULT_HOST, port=DEFAULT_PORT):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    s.settimeout(5.0)
    return s


def send_cmd(sock, cmd_dict):
    line = json.dumps(cmd_dict) + "\n"
    sock.sendall(line.encode())
    buf = b""
    while b"\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return json.loads(buf.decode().strip())


def pretty_regs(resp):
    if not resp.get("ok"):
        return str(resp)
    lines = [f"  PC: {resp['pc']}  HI: {resp['hi']}  LO: {resp['lo']}"]
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


def run_command(sock, args):
    if not args:
        return
    cmd = args[0].lower()

    if cmd == "ping":
        return pretty_json(send_cmd(sock, {"cmd": "ping"}))
    elif cmd == "frame":
        return pretty_json(send_cmd(sock, {"cmd": "frame"}))
    elif cmd in ("regs", "get_registers"):
        return pretty_regs(send_cmd(sock, {"cmd": "get_registers"}))
    elif cmd == "read":
        addr = args[1] if len(args) > 1 else "0x80010000"
        length = int(args[2]) if len(args) > 2 else 16
        return pretty_json(send_cmd(sock, {"cmd": "read_ram", "addr": addr, "len": length}))
    elif cmd == "write":
        if len(args) < 3:
            return "Usage: write <addr> <hex>"
        return pretty_json(send_cmd(sock, {"cmd": "write_ram", "addr": args[1], "hex": args[2]}))
    elif cmd == "scratch":
        addr = args[1] if len(args) > 1 else "0x1F800000"
        length = int(args[2]) if len(args) > 2 else 16
        return pretty_json(send_cmd(sock, {"cmd": "read_scratch", "addr": addr, "len": length}))
    elif cmd == "gpu":
        return pretty_json(send_cmd(sock, {"cmd": "gpu_state"}))
    elif cmd == "overlay":
        return pretty_json(send_cmd(sock, {"cmd": "overlay_state"}))
    elif cmd == "watch":
        if len(args) < 2:
            return "Usage: watch <addr>"
        return pretty_json(send_cmd(sock, {"cmd": "watch", "addr": args[1]}))
    elif cmd == "unwatch":
        if len(args) < 2:
            return "Usage: unwatch <addr>"
        return pretty_json(send_cmd(sock, {"cmd": "unwatch", "addr": args[1]}))
    elif cmd == "pause":
        return pretty_json(send_cmd(sock, {"cmd": "pause"}))
    elif cmd in ("continue", "c"):
        return pretty_json(send_cmd(sock, {"cmd": "continue"}))
    elif cmd == "step":
        n = int(args[1]) if len(args) > 1 else 1
        return pretty_json(send_cmd(sock, {"cmd": "step", "count": n}))
    elif cmd == "run_to":
        if len(args) < 2:
            return "Usage: run_to <frame>"
        return pretty_json(send_cmd(sock, {"cmd": "run_to_frame", "frame": int(args[1])}))
    elif cmd == "history":
        return pretty_json(send_cmd(sock, {"cmd": "history"}))
    elif cmd == "get_frame":
        if len(args) < 2:
            return "Usage: get_frame <n>"
        return pretty_json(send_cmd(sock, {"cmd": "get_frame", "frame": int(args[1])}))
    elif cmd == "range":
        if len(args) < 3:
            return "Usage: range <start> <end>"
        return pretty_json(send_cmd(sock, {"cmd": "frame_range", "start": int(args[1]), "end": int(args[2])}))
    elif cmd == "ts":
        if len(args) < 3:
            return "Usage: ts <start> <end>"
        return pretty_json(send_cmd(sock, {"cmd": "frame_timeseries", "start": int(args[1]), "end": int(args[2])}))
    elif cmd == "input":
        if len(args) < 2:
            return "Usage: input <buttons_hex>"
        return pretty_json(send_cmd(sock, {"cmd": "set_input", "buttons": args[1]}))
    elif cmd == "clear_input":
        return pretty_json(send_cmd(sock, {"cmd": "clear_input"}))
    elif cmd == "quit":
        return pretty_json(send_cmd(sock, {"cmd": "quit"}))
    else:
        # Try sending as raw command
        return pretty_json(send_cmd(sock, {"cmd": cmd}))


def main():
    try:
        sock = connect()
    except ConnectionRefusedError:
        print(f"Cannot connect to {DEFAULT_HOST}:{DEFAULT_PORT} — is the game running?")
        sys.exit(1)

    if len(sys.argv) > 1:
        # Single command mode
        result = run_command(sock, sys.argv[1:])
        if result:
            print(result)
        sock.close()
        return

    # Interactive REPL
    print(f"Connected to PSX debug server at {DEFAULT_HOST}:{DEFAULT_PORT}")
    print("Type 'help' for commands, Ctrl-C to exit\n")
    try:
        while True:
            try:
                line = input("psx> ").strip()
            except EOFError:
                break
            if not line:
                continue
            if line.lower() == "help":
                print(__doc__)
                continue
            args = line.split()
            result = run_command(sock, args)
            if result:
                print(result)
    except KeyboardInterrupt:
        print()
    finally:
        sock.close()


if __name__ == "__main__":
    main()
