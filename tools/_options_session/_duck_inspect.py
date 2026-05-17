import socket, json, sys
def call(cmd, **kw):
    s = socket.create_connection(('127.0.0.1', 4371), timeout=5)
    p = {'id':1,'cmd':cmd, **kw}
    s.sendall((json.dumps(p)+'\n').encode())
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.decode())

print('ping:', call('ping'))
print()
print('read_ram 0x8009B3D0 len=16:')
r = call('read_ram', addr='0x8009B3D0', len=16)
print('  hex:', r.get('hex'))
print()
print('read_ram 0x801FD950 len=128 (state struct):')
r = call('read_ram', addr='0x801FD950', len=128)
print('  hex:', r.get('hex'))
print()
print('gpu_state:')
print(' ', call('gpu_state'))
print()
print('sio_state:')
print(' ', call('sio_state'))
