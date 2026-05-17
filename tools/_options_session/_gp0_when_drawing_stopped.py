"""Find when runtime stopped drawing polygons.
Sample multiple frames at intervals across the ring."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=10)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    s.settimeout(5); buf=b''; out=None; deadline=time.time()+6
    while time.time() < deadline:
        try: chunk = s.recv(65536)
        except socket.timeout: break
        if not chunk: break
        buf += chunk
        while b'\n' in buf:
            line, _, buf = buf.partition(b'\n')
            try: obj = json.loads(line.decode())
            except: continue
            if obj.get('id') == 1: out = obj; deadline = 0; break
    s.close()
    return out or {'err': 'no resp'}

r = call(4470, 'gpu_ring_stats')
oldest = r['oldest_frame']
newest = r['newest_frame']
print(f"Ring spans frames [{oldest}, {newest}] = {newest-oldest} frames")

# Sample 12 frames spread across the ring
def classify(entries):
    cnt = {'poly': 0, 'rect_tx': 0, 'env': 0, 'fill': 0, 'copy': 0, 'misc': 0}
    for e in entries:
        op = int(e.get('op','0x0'), 16)
        if 0x20 <= op <= 0x3F: cnt['poly'] += 1
        elif op == 0x64 or op == 0x65 or op == 0x74 or op == 0x6C or op == 0x7C: cnt['rect_tx'] += 1
        elif 0x60 <= op <= 0x7F: cnt['rect_tx'] += 1
        elif 0xE0 <= op <= 0xEF: cnt['env'] += 1
        elif op == 0x02: cnt['fill'] += 1
        elif 0x80 <= op <= 0xDF: cnt['copy'] += 1
        else: cnt['misc'] += 1
    return cnt

print()
n_samples = 14
for i in range(n_samples):
    frame = oldest + int((newest - oldest) * i / (n_samples-1))
    r = call(4470, 'gpu_frame_dump', frame=frame, count=2048)
    entries = r.get('entries', [])
    c = classify(entries)
    poly_total = c['poly'] + c['rect_tx']
    flag = '  <-- HAS DRAWS' if poly_total > 0 else ''
    print(f"  frame {frame:6}: total={len(entries):4}  poly={c['poly']:3} rect_tx={c['rect_tx']:3}  fill={c['fill']:2} env={c['env']:3} copy={c['copy']:2}{flag}")
