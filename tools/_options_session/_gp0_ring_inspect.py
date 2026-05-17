"""Inspect runtime's GP0 ring at OPTIONS.
Tally GP0 opcodes; dump a recent frame's draw command stream."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=8)
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
print(f"gpu_ring_stats: {r}")

oldest = r.get('oldest_frame')
newest = r.get('newest_frame')

r = call(4470, 'gpu_opcodes')
ops = r.get('opcodes', {})
print(f"\ngpu_opcodes: {len(ops)} distinct opcodes seen")
GP0_NAMES = {
    0x00: 'NOP', 0x01: 'CLEAR_CACHE', 0x02: 'FILL_RECT',
    0x20: 'POLY_FLAT_TRI', 0x22: 'POLY_FLAT_TRI_T', 0x24: 'POLY_FLAT_TRI_TX', 0x26: 'POLY_FLAT_TRI_TX_T',
    0x28: 'POLY_FLAT_QUAD', 0x2A: 'POLY_FLAT_QUAD_T', 0x2C: 'POLY_FLAT_QUAD_TX', 0x2E: 'POLY_FLAT_QUAD_TX_T',
    0x30: 'POLY_GOUR_TRI', 0x32: 'POLY_GOUR_TRI_T', 0x34: 'POLY_GOUR_TRI_TX', 0x36: 'POLY_GOUR_TRI_TX_T',
    0x38: 'POLY_GOUR_QUAD', 0x3A: 'POLY_GOUR_QUAD_T', 0x3C: 'POLY_GOUR_QUAD_TX', 0x3E: 'POLY_GOUR_QUAD_TX_T',
    0x40: 'LINE_FLAT', 0x48: 'LINE_POLY_FLAT',
    0x60: 'RECT_FLAT', 0x64: 'RECT_FLAT_TX', 0x68: 'RECT_FLAT_1', 0x6C: 'RECT_FLAT_1_TX',
    0x70: 'RECT_FLAT_8', 0x74: 'RECT_FLAT_8_TX', 0x78: 'RECT_FLAT_16', 0x7C: 'RECT_FLAT_16_TX',
    0x80: 'CPY_VRAM_VRAM', 0xA0: 'CPY_CPU_VRAM', 0xC0: 'CPY_VRAM_CPU',
    0xE1: 'DRAW_MODE', 0xE2: 'TEXTURE_WIN', 0xE3: 'DRAW_AREA_TL', 0xE4: 'DRAW_AREA_BR',
    0xE5: 'DRAW_OFFSET', 0xE6: 'MASK_BIT',
}
for op_s, n in sorted(ops.items(), key=lambda x: -x[1]):
    op = int(op_s, 16)
    nm = GP0_NAMES.get(op, '?')
    print(f"  0x{op:02X} {nm:20s}: {n}")

print(f"\nDumping frame {newest}:")
r = call(4470, 'gpu_frame_dump', frame=newest, count=4096)
entries = r.get('entries', [])
print(f"  count: {len(entries)}")

# Look for actual polygon draws — focus on top 1xx opcodes
classify = {'polygon': 0, 'rect': 0, 'line': 0, 'fill': 0, 'env': 0, 'copy': 0, 'misc': 0}
poly_samples = []
for e in entries:
    op = int(e.get('op','0x0'), 16)
    if 0x20 <= op <= 0x3F:
        classify['polygon'] += 1
        if len(poly_samples) < 10:
            poly_samples.append(e)
    elif 0x40 <= op <= 0x5F:
        classify['line'] += 1
    elif 0x60 <= op <= 0x7F:
        classify['rect'] += 1
    elif 0x80 <= op <= 0xDF:
        classify['copy'] += 1
    elif op == 0x02:
        classify['fill'] += 1
    elif 0xE0 <= op <= 0xEF:
        classify['env'] += 1
    else:
        classify['misc'] += 1
print(f"  classify: {classify}")
print(f"\n  Sample polygons (first 10):")
for e in poly_samples:
    op = int(e['op'],16)
    nm = GP0_NAMES.get(op,'?')
    print(f"    seq={e['seq']:6} op={e['op']} ({nm}) n={e['n']} w={e['w']}")
