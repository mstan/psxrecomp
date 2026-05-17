"""Check mem[0x8009B3A0+0xE6] on both backends."""
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

# Read 0x8009B486 (= struct base + 0xE6) halfword
# But read as word at 0x8009B484 to be safe
for port, name in [(4470, 'runtime'), (4371, 'duckstation')]:
    r = call(port, 'read_ram', addr='0x8009B400', len=0x100)
    if r.get('ok'):
        b = bytes.fromhex(r['hex'])
        # +0xE6 from struct base (0x8009B3A0) = byte at offset 0xE6
        # We're reading from 0x8009B400 = struct base + 0x60
        # So mem[+0xE6] = byte at offset (0xE6 - 0x60) = 0x86 in this buffer
        off_in_buf = 0xE6 - 0x60
        v = (b[off_in_buf+1] << 8) | b[off_in_buf]  # halfword LE
        print(f"{name:>12} mem[0x8009B486] (struct+0xE6, halfword) = 0x{v:04X}")
        # also +0x46
        off2 = 0x46 - 0x60
        if off2 < 0:
            # need a different read
            r2 = call(port, 'read_ram', addr='0x8009B3E6', len=1)
            v46 = int(r2['hex'], 16)
            print(f"{name:>12} mem[0x8009B3E6] (struct+0x46, byte) = 0x{v46:02X}")
        else:
            print(f"{name:>12} mem[+0x46] (byte) = 0x{b[off2]:02X}")
        # Also dump 0x60..0x100 of buffer
        print(f"{name:>12} struct[+0x60..+0xFF]:")
        for i in range(0, len(b), 16):
            line = ' '.join(f'{x:02x}' for x in b[i:i+16])
            print(f"             +0x{0x60+i:03X}: {line}")
        print()
