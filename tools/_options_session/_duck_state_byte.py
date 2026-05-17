"""Read the state-machine state byte on DuckStation vs runtime."""
import socket, json

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.decode())

def read_word(port, addr):
    r = call(port, 'read_ram', addr=f'0x{addr:08X}', len=4)
    if not r.get('ok'): return None
    b = bytes.fromhex(r['hex'])
    return int.from_bytes(b, 'little')

for label, port in [('runtime', 4470), ('duckstation', 4371)]:
    print(f"=== {label} (port {port}) ===")
    try:
        p = call(port, 'ping')
        print(f"  ping fr={p.get('frame','?')}")
    except Exception as e:
        print(f"  ping failed: {e}")
        continue

    # Read scratchpad ptr at 0x1F8001D4
    ptr = read_word(port, 0x1F8001D4)
    print(f"  scratchpad[0x1D4] (state struct ptr) = 0x{ptr:08X}")

    # State byte at ptr+0x4E (halfword)
    state_word = read_word(port, ptr + 0x4C)  # word containing 0x4E,0x4F bytes
    state = (state_word >> 16) & 0xFFFF
    print(f"  state @ ptr+0x4E = 0x{state:04X}  ({'normal' if state < 6 else '>=6, handler SKIPS'})")

    # 0x8009B3D4 high half
    field_word = read_word(port, 0x8009B3D4)
    print(f"  mem[0x8009B3D4] = 0x{field_word:08X}  high_half=0x{(field_word>>16)&0xFFFF:04X}  ({'state-0 main path' if (field_word & 0xFFFF0000)==0 else 'state-0 side path (skips!)'})")
    print()
