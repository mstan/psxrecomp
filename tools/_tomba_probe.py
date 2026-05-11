"""Probe tomba-runtime debug server on port 4470 for boot state."""
import socket
import json
import sys


def q(cmd, **kwargs):
    payload = {"id": 1, "cmd": cmd}
    payload.update(kwargs)
    s = socket.create_connection(("127.0.0.1", 4470), timeout=3)
    s.send((json.dumps(payload) + "\n").encode())
    chunks = []
    while True:
        try:
            data = s.recv(16384)
        except socket.timeout:
            break
        if not data:
            break
        chunks.append(data)
        if b"\n" in data:
            break
    s.close()
    return b"".join(chunks).decode(errors="replace").strip()


cmds = sys.argv[1:] if len(sys.argv) > 1 else [
    "ping",
    "cdrom_state",
    "pad_status",
    "dirty_ram_unsupported",
    "irq_state",
]
for c in cmds:
    print(f"=== {c} ===")
    try:
        print(q(c))
    except Exception as e:
        print(f"ERROR: {e!r}")
    print()
