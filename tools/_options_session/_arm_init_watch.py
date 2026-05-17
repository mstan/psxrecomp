"""Arm watchpoints on the init-target addresses (0x8009B3B4 = +0x14) on both
backends. Then we poll for a few seconds to catch any writes happening in
the OPTIONS state."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.decode())

ADDR = 0x8009B3B4  # struct base + 0x14

# DuckStation (port 4371) uses mem_break + mem_hit_last
print("=== DuckStation (port 4371) ===")
call(4371, 'mem_hit_clear')
r = call(4371, 'mem_break', addr=f'0x{ADDR:08X}')
print(f"  mem_break: {r}")

# Runtime (port 4470) uses wtrace_arm
print()
print("=== Runtime (port 4470) ===")
r = call(4470, 'wtrace_arm', lo=f'0x{(ADDR & 0x1FFFFFFF):08X}', hi=f'0x{(ADDR & 0x1FFFFFFF) + 4:08X}')
print(f"  wtrace_arm: {r}")

# Poll for 5 seconds
print()
print("--- Polling for 5 seconds (both backends still at OPTIONS) ---")
end = time.time() + 5
duck_hits = 0
while time.time() < end:
    h = call(4371, 'mem_hit_last')
    if h.get('valid'):
        duck_hits += 1
        print(f"  DUCK HIT: pc={h.get('pc')} ra={h.get('ra')} mem_addr={h.get('mem_addr')}")
        call(4371, 'mem_hit_clear')
    time.sleep(0.1)

print(f"\nDuckStation: {duck_hits} hits on 0x{ADDR:08X}")
# Now dump runtime's wtrace
rt = call(4470, 'wtrace_dump', count=50)
es = rt.get('entries', [])
matching = [e for e in es if int(e.get('addr','0'), 16) == ADDR]
print(f"Runtime: {len(matching)} entries matching 0x{ADDR:08X}")
for e in matching[:10]:
    print(f"  RT HIT: seq={e.get('seq')} pc={e.get('pc')} new={e.get('new')} fr={e.get('frame')}")
