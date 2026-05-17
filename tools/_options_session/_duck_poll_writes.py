"""Poll mem_hit_last on DuckStation to catch the PC writing to 0x8009B3D4."""
import socket, json, time

def call(cmd, **kw):
    s = socket.create_connection(('127.0.0.1', 4371), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.decode())

call('mem_hit_clear')

hits_seen = set()
print("Polling for writes to 0x8009B3D4 (5 sec)...")
end = time.time() + 5
while time.time() < end:
    h = call('mem_hit_last')
    if h.get('valid'):
        key = (h['pc'], h['ra'])
        if key not in hits_seen:
            hits_seen.add(key)
            print(f"  WRITE  pc={h['pc']} ra={h['ra']} mem_addr={h['mem_addr']}")
        call('mem_hit_clear')
    time.sleep(0.005)

print(f"\nDistinct (pc, ra) write sites: {len(hits_seen)}")
for pc, ra in sorted(hits_seen):
    print(f"  pc={pc}  ra={ra}")
