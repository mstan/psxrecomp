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

for port, name in [(4470, 'runtime'), (4371, 'duck')]:
    r = call(port, 'read_ram', addr='0x80090C90', len=16)
    if r.get('ok'):
        b = bytes.fromhex(r['hex'])
        c94 = int.from_bytes(b[4:8], 'little')
        c98 = int.from_bytes(b[8:12], 'little')
        c9e = b[14]
        print(f"{name:>8}: mem[0x80090C9E] byte = 0x{c9e:02X} ({c9e}) -> gate: {'work+ChangeTh' if c9e >= 2 else 'ChangeTh only'}")
        print(f"{name:>8}: mem[0x80090C94] ptr  = 0x{c94:08X}")
        print(f"{name:>8}: mem[0x80090C98] ptr  = 0x{c98:08X}")
        print()

# Also follow the indirect: mem[mem[0x80090C94]+0x3C]
print("Indirect dispatch target mem[mem[0x80090C94]+0x3C]:")
for port, name in [(4470, 'runtime'), (4371, 'duck')]:
    r1 = call(port, 'read_ram', addr='0x80090C94', len=4)
    c94 = int.from_bytes(bytes.fromhex(r1['hex']), 'little')
    r2 = call(port, 'read_ram', addr=f'0x{c94+0x3C:08X}', len=4)
    fn = int.from_bytes(bytes.fromhex(r2['hex']), 'little')
    print(f"  {name:>8}: mem[0x{c94:08X}+0x3C] = 0x{fn:08X}  (function called at 0x8005EBA4)")
