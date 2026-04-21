#!/usr/bin/env python3
"""Tally MMIO trace entries by address."""
import json, socket, sys
from collections import Counter

host = "127.0.0.1"
port = 4370

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect((host, port))
s.sendall(b'{"cmd":"mmio_dump"}\n')
buf = b""
while b"\n" not in buf:
    buf += s.recv(65536)
s.close()

data = json.loads(buf.decode().strip())
entries = data.get("entries", [])
total = data.get("total", 0)
avail = data.get("available", 0)
emitted = data.get("emitted", 0)

print(f"MMIO trace: {emitted} emitted / {avail} available / {total} total writes")

addrs = Counter(e["addr"] for e in entries)
print(f"\nBy address ({len(addrs)} unique):")
for a, c in addrs.most_common(30):
    print(f"  {a}: {c} writes")

# Show SIO range specifically
sio = [e for e in entries if e["addr"] >= "0x1F801040" and e["addr"] <= "0x1F80105F"]
print(f"\nSIO writes (0x1F801040-5F): {len(sio)}")
for e in sio[:20]:
    print(f"  frame={e['frame']} {e['addr']}={e['val']} func={e['func']} ra={e['ra']}")
