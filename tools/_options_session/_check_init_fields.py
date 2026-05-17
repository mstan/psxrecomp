"""Are the missing-init fields STATICALLY missing on runtime, or do they cycle?
Sample several offsets over 2 sec on both backends."""
import socket, json, time

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
    return int.from_bytes(bytes.fromhex(r['hex']), 'little') if r.get('ok') else None

# Critical offsets where runtime is suspected wrong
fields = [
    (0x8009B3A0, "+0x00 first pointer"),
    (0x8009B3A4, "+0x04 second pointer"),
    (0x8009B3A8, "+0x08 third pointer"),
    (0x8009B3B4, "+0x14 fn ptr 1"),
    (0x8009B3B8, "+0x18 fn ptr 2"),
    (0x8009B3C0, "+0x20 pointer"),
]

print(f"{'addr':>14}  {'runtime':>40}  {'duckstation':>40}")
print(f"{'':14}  {'(5 samples)':>40}  {'(5 samples)':>40}")
for addr, label in fields:
    rt_vals = [read_word(4470, addr) for _ in range(5)]
    du_vals = [read_word(4371, addr) for _ in range(5)]
    rt_str = ','.join(f'{v:08X}' for v in rt_vals)
    du_str = ','.join(f'{v:08X}' for v in du_vals)
    rt_static = len(set(rt_vals)) == 1
    du_static = len(set(du_vals)) == 1
    print(f"  0x{addr:08X}  RT[{'static' if rt_static else 'cyclic'}]: {rt_str}")
    print(f"  {'':10}  DU[{'static' if du_static else 'cyclic'}]: {du_str}  ({label})")
    print()
