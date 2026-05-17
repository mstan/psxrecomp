"""On DuckStation: read current slot states, arm mem_break on slot 0's mem[+0],
poll for writes. Each write tells us a PC that transitions slot state."""
import socket, json, time

def call(cmd, **kw):
    s = socket.create_connection(('127.0.0.1', 4371), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while b'\n' not in buf:
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    line, _, _ = buf.partition(b'\n')
    return json.loads(line.decode())

print(f"ping: {call('ping')}")

# Read current slot states (all 3 slots, byte at offset 0 of each)
for slot, addr in [(0, 0x801FD800), (1, 0x801FD870), (2, 0x801FD8E0)]:
    r = call('read_ram', addr=f'0x{addr:08X}', len=4)
    if 'hex' not in r:
        print(f"  slot {slot} mem[0x{addr:08X}]: ERR {r}")
        continue
    b = bytes.fromhex(r['hex'])
    halfword = b[0] | (b[1]<<8)
    print(f"  slot {slot} mem[0x{addr:08X}+0] (halfword) = 0x{halfword:04X}")

# Arm mem_break on slot 0's mem[+0]
print()
print(f"mem_hit_clear: {call('mem_hit_clear')}")
print(f"mem_break 0x801FD800: {call('mem_break', addr='0x801FD800')}")

# Poll for 5 sec, collect distinct (pc, ra) writes
print()
print("Polling for 5 sec...")
hits = []
seen_pcs = set()
end = time.time() + 5
while time.time() < end:
    h = call('mem_hit_last')
    if h.get('valid'):
        key = (h.get('pc'), h.get('ra'))
        if key not in seen_pcs:
            seen_pcs.add(key)
            hits.append(h)
            print(f"  WRITE pc={h.get('pc')} ra={h.get('ra')} mem_addr={h.get('mem_addr')}")
        call('mem_hit_clear')
    time.sleep(0.02)

print(f"\nDistinct write PCs: {len(seen_pcs)}")
for h in hits:
    print(f"  pc={h.get('pc')} ra={h.get('ra')}")
