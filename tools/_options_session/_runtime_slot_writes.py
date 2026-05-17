"""Watch runtime writes to mem[0x801FD800..0x801FD810] for 4 seconds.
If runtime never writes to it (only the BIOS-ChangeTh-save path), that
confirms divergence: DuckStation has state-transition writes there,
runtime does not."""
import socket, json, time, collections

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

r = call(4470, 'wtrace_clear')
print(f"wtrace_clear: {r}")
r = call(4470, 'wtrace_arm', lo='0x801FD800', hi='0x801FD810')
print(f"wtrace_arm:   {r}")

print("Sampling 4 sec...")
time.sleep(4)

r = call(4470, 'wtrace_dump', count=2000)
entries = r.get('entries', [])
total = r.get('total', 0)
print(f"\n=== runtime: total={total} entries_returned={len(entries)} ===")

# Filter to our address range
hits = []
for e in entries:
    addr_s = e.get('addr', '0')
    addr = int(addr_s, 16) if isinstance(addr_s, str) else addr_s
    if 0x001FD800 <= addr <= 0x001FD810 or 0x801FD800 <= addr <= 0x801FD810:
        hits.append(e)

print(f"Hits in [0x801FD800..0x801FD810]: {len(hits)}")
seen = collections.Counter()
for e in hits[:40]:
    key = (e.get('ra'), e.get('new'))
    seen[key] += 1
    print(f"  addr={e.get('addr')} old={e.get('old')} new={e.get('new')} ra={e.get('ra')}")

call(4470, 'wtrace_clear')
