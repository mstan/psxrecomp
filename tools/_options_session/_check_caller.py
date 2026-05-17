"""Check if 0x80028B34 (caller of 0x80068DDC) fires on runtime.
If not, we go further upstream."""
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

# Arm fntrace on 0x80028B34 on runtime
call(4470, 'fntrace_arm', target='0x80028B34')
# Also arm 0x80068DDC (the immediate caller of init function)
call(4470, 'fntrace_arm', target='0x80068DDC')
# And 0x80022458 (caller of 0x80028B34)
call(4470, 'fntrace_arm', target='0x80022458')
# And 0x8006A0C0 itself
call(4470, 'fntrace_arm', target='0x8006A0C0')

# Wait 3 sec
print("Waiting 3 sec for hits...")
time.sleep(3)

# Dump
d = call(4470, 'fntrace_dump', count=500)
es = d.get('entries', [])
print(f"Total entries: {len(es)}")

import collections
funcs = collections.Counter(e.get('target') for e in es)
print("Top hit counts:")
for f, c in funcs.most_common(10):
    print(f"  {f}: {c}")
