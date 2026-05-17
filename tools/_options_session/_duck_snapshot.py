"""Snapshot DuckStation at OPTIONS — golden oracle data."""
import socket, json, struct, sys

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

def read_word(addr):
    r = call('read_ram', addr=f'0x{addr:08X}', len=4)
    return int.from_bytes(bytes.fromhex(r['hex']), 'little') if r.get('ok') else None

def read_bytes(addr, n):
    r = call('read_ram', addr=f'0x{addr:08X}', len=n)
    return bytes.fromhex(r['hex']) if r.get('ok') else None

p = call('ping')
print(f"=== DuckStation oracle @ frame {p['frame']} (OPTIONS) ===")
print()

# Critical state-machine readings
ptr = read_word(0x1F8001D4)
print(f"  scratchpad[0x1D4] (state struct ptr) = 0x{ptr:08X}")

if ptr:
    state_word = read_word(ptr + 0x4C)
    state = (state_word >> 16) & 0xFFFF
    print(f"  state @ ptr+0x4E = 0x{state:04X}")

print()
print(f"  mem[0x80097520..0x8009752F] (fn ptr table):")
b = read_bytes(0x80097520, 16)
if b:
    for i in range(0, 16, 4):
        w = struct.unpack_from('<I', b, i)[0]
        print(f"    [0x{0x80097520+i:08X}] = 0x{w:08X}")
print()
print(f"  mem[0x8009B3A0..0x8009B43F] (state-0 helper return ptr struct):")
b = read_bytes(0x8009B3A0, 0xA0)
if b:
    for i in range(0, len(b), 16):
        line = ' '.join(f'{x:02x}' for x in b[i:i+16])
        nz = sum(1 for x in b[i:i+16] if x != 0)
        print(f"    +0x{i:03X}: {line} ({nz} nz)")
print()
print(f"  CRITICAL: mem[0x8009B3D4] = 0x{read_word(0x8009B3D4):08X}")
print(f"           high half = 0x{(read_word(0x8009B3D4) >> 16) & 0xFFFF:04X}  ({'main path (state advances)' if (read_word(0x8009B3D4) & 0xFFFF0000) == 0 else 'side path (handler skips)'})")

print()
print("  GPU state:")
g = call('gpu_state')
print(f"    disp=({g.get('display_area_x')},{g.get('display_area_y')}) draw_offset=({g.get('draw_offset_x')},{g.get('draw_offset_y')})")
print(f"    display_enable={g.get('display_enable')} gpustat={g.get('gpustat')}")

# Now arm mem_break to catch future writes
print()
print("Arming mem_break on 0x8009B3D4...")
print(f"  mem_hit_clear: {call('mem_hit_clear')}")
print(f"  mem_break:     {call('mem_break', addr='0x8009B3D4')}")
