import socket, json
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

print('ping:', call('ping'))
print()
print('read_ram(0x1F8001D4, 16):',  call('read_ram', addr='0x1F8001D4', len=16))
print()
print('read_scratch(0x1D4, 16):',  call('read_scratch', addr='0x1D4', len=16))
print()
print('read_ram(0x1F800000, 32) (scratchpad start):')
print(' ', call('read_ram', addr='0x1F800000', len=32))
