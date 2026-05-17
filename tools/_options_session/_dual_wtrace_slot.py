"""Compare write activity at mem[0x801FD800] between runtime and DuckStation.

Both backends should support wtrace_addr / wtrace_dump.
If DuckStation writes (esp. value=2 or 3 transitions) and runtime never does,
that confirms the upstream code-path divergence."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while b'\n' not in buf:
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    line, _, _ = buf.partition(b'\n')
    return json.loads(line.decode())

PORTS = [(4470, 'runtime'), (4371, 'duck')]

# Clear any prior wtrace state
for port, name in PORTS:
    r1 = call(port, 'wtrace_clear')
    r2 = call(port, 'wtrace_addr', addr='0x801FD800')
    print(f"  {name}: wtrace_clear={r1.get('ok')} wtrace_addr={r2.get('ok')} err={r2.get('err')}")

print()
print("Sampling for 3 sec...")
time.sleep(3)

for port, name in PORTS:
    r = call(port, 'wtrace_dump', count=64)
    entries = r.get('entries') or r.get('hits') or []
    print(f"\n=== {name} (port {port}): {len(entries)} write entries ===")
    seen = {}
    for e in entries[:40]:
        pc = e.get('pc') or e.get('store_pc')
        val = e.get('val') or e.get('value')
        key = (pc, val)
        seen[key] = seen.get(key, 0) + 1
    for (pc, val), n in sorted(seen.items(), key=lambda x: -x[1])[:20]:
        print(f"  pc={pc} val={val} count={n}")
    if not entries:
        print("  (empty)")

# Clean up
for port, name in PORTS:
    call(port, 'wtrace_clear')
