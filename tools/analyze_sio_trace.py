#!/usr/bin/env python3
"""Analyze SIO state writes from trace."""
import json, socket

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", 4370))
s.sendall(b'{"cmd":"wtrace_dump","addr_lo":"0x66940","addr_hi":"0x66950"}\n')
buf = b""
while b"\n" not in buf:
    buf += s.recv(65536)
s.close()

data = json.loads(buf.decode().strip())
entries = data.get("entries", [])

print(f"SIO state writes: {len(entries)}")
print(f"\nUnique func_addr values:")
from collections import Counter
funcs = Counter(e["func"] for e in entries)
for f, c in funcs.most_common():
    print(f"  {f}: {c} writes")

print(f"\nUnique $ra values:")
ras = Counter(e["ra"] for e in entries)
for r, c in ras.most_common():
    print(f"  {r}: {c} writes")

print(f"\nFirst 12 entries (2 cycles):")
for e in entries[:12]:
    print(f"  frame={e['frame']:5d} {e['addr']} old={e['old']} new={e['new']} ra={e['ra']} func={e['func']}")

# Check for any writes by the SIO command dispatcher (1FC1E1CC)
sio_cmd_writes = [e for e in entries if e["func"] == "0x1FC1E1CC"]
print(f"\nWrites from SIO command dispatcher (0x1FC1E1CC): {len(sio_cmd_writes)}")
