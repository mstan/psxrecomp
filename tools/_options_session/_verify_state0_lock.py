"""Verify the previously-identified state-0 lock still holds.
Check mem[0x8009B3D4], state byte, scratchpad ptr, jump table."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    s.settimeout(3); buf=b''; out=None; deadline=time.time()+4
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

def rd32(port, addr):
    r = call(port, 'read_ram', addr=f'0x{addr:08X}', len=4)
    if 'hex' not in r: return None
    return int.from_bytes(bytes.fromhex(r['hex']), 'little')

# Drain duck
for _ in range(5): call(4371, 'mem_hit_clear')

print("== mem[0x1F8001D4] (scratchpad ptr to state struct) ==")
print(f"  runtime: 0x{rd32(4470, 0x1F8001D4):08X}")
print(f"  duck:    0x{rd32(4371, 0x1F8001D4):08X}")

print()
print("== mem[0x8009B3D4] (struct + 0x34, holds 'EC' on runtime) ==")
r_val = rd32(4470, 0x8009B3D4)
d_val = rd32(4371, 0x8009B3D4)
print(f"  runtime: 0x{r_val:08X}")
print(f"  duck:    0x{d_val:08X}")
print(f"  diff:    {'YES' if r_val != d_val else 'no'}")

print()
print("== mem[0x8009B3A0..+0x80] struct contents ==")
print("  runtime:")
r = call(4470, 'read_ram', addr='0x8009B3A0', len=0x80)
if 'hex' in r:
    b = bytes.fromhex(r['hex'])
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        words = [int.from_bytes(chunk[k:k+4],'little') for k in range(0, 16, 4)]
        ascii_repr = ''.join(chr(x) if 32 <= x < 127 else '.' for x in chunk)
        print(f"    +0x{i:02X}: {' '.join(f'{w:08X}' for w in words)}  |{ascii_repr}|")

print("  duck:")
r = call(4371, 'read_ram', addr='0x8009B3A0', len=0x80)
if 'hex' in r:
    b = bytes.fromhex(r['hex'])
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        words = [int.from_bytes(chunk[k:k+4],'little') for k in range(0, 16, 4)]
        ascii_repr = ''.join(chr(x) if 32 <= x < 127 else '.' for x in chunk)
        print(f"    +0x{i:02X}: {' '.join(f'{w:08X}' for w in words)}  |{ascii_repr}|")

print()
print("== mem[0x80097524] (fn-ptr) ==")
print(f"  runtime: 0x{rd32(4470, 0x80097524):08X}  (expected 0x8006B028)")
print(f"  duck:    0x{rd32(4371, 0x80097524):08X}")
