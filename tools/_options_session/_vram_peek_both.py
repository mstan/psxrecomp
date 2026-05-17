"""Peek at both double-buffer halves on runtime + duck.
Runtime claims display=(384,256). Both halves should be in y>=256, x=[384..1023]."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    s.settimeout(3)
    buf = b''
    out = None
    deadline = time.time() + 4
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout: break
        if not chunk: break
        buf += chunk
        while b'\n' in buf:
            line, _, buf = buf.partition(b'\n')
            try:
                obj = json.loads(line.decode())
            except Exception: continue
            if obj.get('id') == 1:
                out = obj
                deadline = 0
                break
    s.close()
    return out or {'err': 'no resp'}

def vram_stats(port, x, y, w=64, h=8):
    r = call(port, 'vram_peek', x=x, y=y, w=w, h=h)
    if 'hex' not in r:
        return None, r
    h_s = r['hex']
    pixels = [int(h_s[i*4:i*4+4], 16) for i in range(len(h_s)//4)]
    nz = sum(1 for p in pixels if p != 0)
    return (nz, len(pixels), len(set(pixels)), pixels[:8]), r

# Drain duck
for _ in range(3):
    call(4371, 'mem_hit_clear')

REGIONS = [
    ('buffer_A', 384, 256, 64, 8),
    ('buffer_B', 704, 256, 64, 8),
    ('tex_page_topL', 0, 0, 64, 8),
    ('tex_page_320_0', 320, 0, 64, 8),
    ('tex_page_640_0', 640, 0, 64, 8),
    ('tex_page_0_128', 0, 128, 64, 8),
    ('clut_320_240', 320, 240, 64, 8),
]
PORTS = [(4470, 'runtime'), (4371, 'duck')]
for name, x, y, w, h in REGIONS:
    print(f"\n=== {name} (vram[{x},{y}] {w}x{h}) ===")
    for port, pname in PORTS:
        s, r = vram_stats(port, x, y, w, h)
        if s:
            nz, total, uniq, sample = s
            print(f"  {pname}: nonzero={nz}/{total} unique={uniq} sample={[hex(p) for p in sample]}")
        else:
            print(f"  {pname}: ERR {r}")
