"""FUN_8006A0C0 is the init that writes struct+0x14/0x18/0x20/0x46.
It bails early if mem[0x8009752C](a0) returns non-zero.
Check that fn ptr + other init-related fields."""
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

for _ in range(5): call(4371, 'mem_hit_clear')

# The bailout fn ptr
print("== mem[0x8009752C] (gate fn ptr — called by 0x8006A0C0) ==")
print(f"  runtime: 0x{rd32(4470, 0x8009752C):08X}")
print(f"  duck:    0x{rd32(4371, 0x8009752C):08X}")

# Surrounding fn-ptr area at 0x80097500..0x80097540
print("\n== mem[0x80097500..0x80097540] fn-ptr area ==")
for port, name in [(4470, 'runtime'), (4371, 'duck')]:
    r = call(port, 'read_ram', addr='0x80097500', len=0x40)
    if 'hex' in r:
        b = bytes.fromhex(r['hex'])
        print(f"  {name}:")
        for i in range(0, len(b), 16):
            words = [int.from_bytes(b[i+k:i+k+4],'little') for k in range(0, 16, 4)]
            print(f"    +0x{i:02X}: {' '.join(f'{w:08X}' for w in words)}")

# Check the struct's "is alloc'd" indicator -- looks like +0x10 is a "next ptr" or "header"
# Per the disasm of 0x8006A0C0, the gate fn was called with a0=struct. Maybe it's an
# allocator that returns 0 on success, nonzero on already-alloc'd
