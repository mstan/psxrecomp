import socket, json
def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while b'\n' not in buf:
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.partition(b'\n')[0].decode())

for cmd, kw in [
    ('wtrace_arm', {'lo': '0x801FD800', 'hi': '0x801FD802'}),
    ('wtrace_arm', {'lo': '0x801FD800'}),
    ('wtrace_addr', {'addr': '0x801FD800'}),
    ('mem_break', {'addr': '0x801FD800'}),
    ('mem_write_break', {'addr': '0x801FD800'}),
    ('wtrace_pc', {'pc': '0x800173FC'}),
    ('wtrace_dump', {}),
    ('fntrace_arm', {'pc': '0x800173B0'}),
    ('fntrace_dump', {}),
    ('fn_entry_arm', {'pc': '0x800173B0'}),
]:
    r = call(4470, cmd, **kw)
    print(f"  {cmd} {kw}: {str(r)[:160]}")
