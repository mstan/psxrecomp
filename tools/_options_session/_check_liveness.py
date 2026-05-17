import socket, json
def call(port, cmd, **kw):
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=2)
        s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
        buf = b''
        while not buf.endswith(b'\n'):
            c = s.recv(65536)
            if not c: break
            buf += c
        s.close()
        return json.loads(buf.decode())
    except Exception as e:
        return {'err': str(e)}
for port, name in [(4470, 'runtime'), (4371, 'duck'), (4380, 'beetle')]:
    r = call(port, 'ping')
    print(f'{name:>8} (port {port}): {r}')
