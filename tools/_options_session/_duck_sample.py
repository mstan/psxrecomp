"""Take 20 consecutive snapshots of mem[0x9B3D4] to see if it actually changes
or if the prior 'changes' were just one-off oddities."""
import socket, json, time

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

prev = None
print(f"{'i':>3} {'frame':>6} {'mem[0x8009B3D4]':>18} {'changed':>8}")
for i in range(20):
    p = call('ping')
    r = call('read_ram', addr='0x8009B3D4', len=4)
    h = r.get('hex')
    changed = '*' if (prev and prev != h) else ''
    print(f"{i:>3} {p['frame']:>6} {h:>18} {changed:>8}")
    prev = h
    time.sleep(0.2)
