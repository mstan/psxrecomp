"""Side-by-side dump of mem[0x8009B3A0..0x8009B440] on runtime vs DuckStation."""
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

def read(port, addr, n):
    r = call(port, 'read_ram', addr=f'0x{addr:08X}', len=n)
    return bytes.fromhex(r['hex'])

BASE = 0x8009B3A0
N = 0xA0

rt = read(4470, BASE, N)
du = read(4371, BASE, N)

print(f"{'off':>5}  {'runtime':>48}  {'duckstation':>48}")
for i in range(0, N, 16):
    rt_hex = ' '.join(f'{x:02x}' for x in rt[i:i+16])
    du_hex = ' '.join(f'{x:02x}' for x in du[i:i+16])
    rt_full = f"+0x{i:03X}"
    diff_marker = '  '
    for j in range(16):
        if rt[i+j] != du[i+j]:
            diff_marker = '!!'
            break
    print(f"  {rt_full:>5}  RT: {rt_hex}  {diff_marker}  BE: {du_hex}")
