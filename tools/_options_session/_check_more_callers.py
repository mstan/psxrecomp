"""Check if 0x800223E0 fires on runtime, plus its callers 0x80019898 and 0x8001A5D4
(actually, the FUNCTIONS containing those PCs)."""
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

# Arm fntrace at every level we care about
targets = [
    '0x800223E0',   # function containing call to 0x80028B34
    '0x80019898',   # caller PC of 0x800223E0
    '0x8001A5D4',   # other caller PC of 0x800223E0
    '0x80028D70',   # also called from 0x800223E0 — see if these run
    '0x8006A0C0',   # the init we know doesn't fire
]
for t in targets:
    call(4470, 'fntrace_arm', target=t)

print(f"Armed {len(targets)} targets. Waiting 5 sec...")
time.sleep(5)

d = call(4470, 'fntrace_dump', count=500)
es = d.get('entries', [])
print(f"Total entries: {len(es)}")

import collections
funcs = collections.Counter(e.get('target') for e in es)
print("Hit counts:")
for t in targets:
    addr_int = int(t, 16)
    matches = [k for k in funcs if int(k, 16) == addr_int]
    n = sum(funcs[k] for k in matches)
    label = '[' + t + ']'
    print(f"  {label:>14} hits: {n}")
