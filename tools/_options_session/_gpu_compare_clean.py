"""Clean compare of GPU state at OPTIONS.
Drain pending events from each socket first."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    # drain async events; wait for response whose id matches
    s.settimeout(2)
    buf = b''
    out = None
    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        buf += chunk
        while b'\n' in buf:
            line, _, buf = buf.partition(b'\n')
            try:
                obj = json.loads(line.decode())
            except Exception:
                continue
            if obj.get('id') == 1:
                out = obj
                deadline = 0
                break
    s.close()
    return out or {'err': 'no id=1 response'}

# Clear DuckStation mem_break first (use a session that drains events)
print("Clearing duck mem_hit...")
for _ in range(5):
    call(4371, 'mem_hit_clear')

PORTS = [(4470, 'runtime'), (4371, 'duck')]

print("\n=== gpu_state ===")
for port, name in PORTS:
    r = call(port, 'gpu_state')
    print(f"  {name}: {r}")

# screenshot to PNG for visual check
print("\n=== screenshot ===")
for port, name in PORTS:
    r = call(port, 'screenshot')
    if r.get('ok') and 'hex' in r:
        w, h = r['width'], r['height']
        b = bytes.fromhex(r['hex'])
        # Quick brightness check: count non-zero pixels
        nz = sum(1 for x in b if x != 0)
        print(f"  {name}: {w}x{h} fmt={r.get('format')} non_zero_bytes={nz}/{len(b)} ({100*nz/max(1,len(b)):.1f}%)")
        with open(f'opt_screenshot_{name}.bin', 'wb') as fh:
            fh.write(b)
        print(f"    saved opt_screenshot_{name}.bin")
    else:
        print(f"  {name}: {r}")
