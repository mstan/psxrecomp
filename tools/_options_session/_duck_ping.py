import socket, json
s = socket.create_connection(('127.0.0.1', 4371), timeout=5)
s.sendall(b'{"id":1,"cmd":"ping"}\n')
buf = b''
while not buf.endswith(b'\n'):
    chunk = s.recv(65536)
    if not chunk: break
    buf += chunk
s.close()
print(buf.decode())
