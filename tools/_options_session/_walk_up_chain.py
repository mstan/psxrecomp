"""Walk up the call chain: arm fntrace at function entries to see who fires."""
import socket, json, time, collections

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

# Function entries to test, top → bottom
targets = [
    ('0x800163B0', 'tomba_main'),
    ('0x80019844', 'FUN_80019844 (caller of 0x800223E0 via 0x80019898)'),
    ('0x8001A51C', 'FUN_8001A51C (caller of 0x800223E0 via 0x8001A5D4)'),
    ('0x8001A328', 'FUN_8001A328 (calls chain handler 0x800E75CC)'),
    ('0x800223E0', '0x800223E0 (calls 0x80028B34)'),
    ('0x80028B34', '0x80028B34 (calls 0x80068DDC)'),
    ('0x80068DDC', '0x80068DDC (calls 0x8006A0C0 = init)'),
    ('0x8006A0C0', '0x8006A0C0 (the init function)'),
]

for t, _ in targets:
    call(4470, 'fntrace_arm', target=t)

print(f"Armed {len(targets)} targets. Waiting 5 sec...")
time.sleep(5)

d = call(4470, 'fntrace_dump', count=5000)
es = d.get('entries', [])
funcs = collections.Counter(e.get('target') for e in es)

print(f"{'target':>14} {'hits':>6}  label")
for t, label in targets:
    t_int = int(t, 16)
    n = sum(funcs[k] for k in funcs if int(k, 16) == t_int)
    print(f"  {t:>12} {n:>6}  {label}")
