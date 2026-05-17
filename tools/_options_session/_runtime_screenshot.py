"""Pull runtime screenshot (multi-line response: meta + 224 row JSON lines)."""
import socket, json, time, sys

PORT = 4470 if len(sys.argv) < 2 else int(sys.argv[1])

s = socket.create_connection(('127.0.0.1', PORT), timeout=15)
s.sendall((json.dumps({'id': 1, 'cmd': 'screenshot'}) + '\n').encode())
s.settimeout(8)
buf = b''
meta = None
rows = []
deadline = time.time() + 8
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
        if 'width' in obj and 'height' in obj and 'row' not in obj:
            meta = obj
        elif 'row' in obj:
            rows.append(obj)
            if meta and len(rows) >= meta.get('height', 0):
                deadline = 0
                break
s.close()

print(f"meta: {meta}")
print(f"rows received: {len(rows)}")
if not meta:
    sys.exit(1)

W, H = meta['width'], meta['height']
# Build pixel array
pixels = []
for r in rows[:H]:
    hex_s = r.get('hex', '')
    row_pixels = []
    for x in range(W):
        if 4*x + 4 <= len(hex_s):
            p = int(hex_s[4*x:4*x+4], 16)
            row_pixels.append(p)
    pixels.append(row_pixels)

# Statistics
total = 0
nonzero = 0
unique = set()
hist = {}
for row in pixels:
    for p in row:
        total += 1
        if p != 0:
            nonzero += 1
        unique.add(p)
        hist[p] = hist.get(p, 0) + 1
print(f"total pixels:    {total}")
print(f"non-zero pixels: {nonzero}  ({100*nonzero/max(1,total):.2f}%)")
print(f"unique colors:   {len(unique)}")
print(f"top 10 colors:")
for p, n in sorted(hist.items(), key=lambda x: -x[1])[:10]:
    # RGB555 -> R,G,B
    r = (p & 0x1F) << 3
    g = ((p >> 5) & 0x1F) << 3
    b = ((p >> 10) & 0x1F) << 3
    print(f"  0x{p:04X} -> rgb({r:3},{g:3},{b:3})  {n} px ({100*n/total:.1f}%)")
