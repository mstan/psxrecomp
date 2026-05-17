"""Compare GPU activity at OPTIONS between runtime and DuckStation.
Look at: frame rate, recent GPU commands."""
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
    return json.loads(buf.partition(b'\n')[0].decode())

PORTS = [(4470, 'runtime'), (4371, 'duck')]

# Frame rate
print("=== Frame rate over 2 sec ===")
for port, name in PORTS:
    f0 = call(port, 'ping').get('frame', 0)
    time.sleep(2)
    f1 = call(port, 'ping').get('frame', 0)
    print(f"  {name}: {f0} -> {f1} (d={f1-f0} frames in 2s = {(f1-f0)/2:.1f} fps)")

# probe gpu/display commands
print("\n=== Display state probes ===")
for port, name in PORTS:
    print(f"--- {name} ---")
    for cmd, kw in [
        ('gpu_stats', {}),
        ('gpu_state', {}),
        ('gpu_status', {}),
        ('gpu_ring', {'count': 10}),
        ('gp0_ring', {'count': 10}),
        ('display_state', {}),
        ('vram_stat', {}),
        ('screenshot', {}),
    ]:
        try:
            r = call(port, cmd, **kw)
            txt = str(r)[:200]
            print(f"  {cmd}: {txt}")
        except Exception as e:
            print(f"  {cmd}: EX {e}")
