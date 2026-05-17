import socket, json, time
def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    s.settimeout(3); buf=b''; out=None; deadline=time.time()+4
    while time.time() < deadline:
        try: chunk = s.recv(65536)
        except socket.timeout: break
        if not chunk: break
        buf += chunk
        while b'\n' in buf:
            line, _, buf = buf.partition(b'\n')
            try: obj = json.loads(line.decode())
            except: continue
            if obj.get('id') == 1: out = obj; deadline = 0; break
    s.close()
    return out or {'err': 'no resp'}

for _ in range(3): call(4371, 'mem_hit_clear')

# Probe what VRAM-read commands duck supports
for cmd, kw in [
    ('vram_peek', {'x':384,'y':256,'w':64,'h':8}),
    ('vram_read', {'x':384,'y':256,'w':64,'h':8}),
    ('peek_vram', {'x':384,'y':256,'w':64,'h':8}),
    ('emu_vram_peek', {'x':384,'y':256,'w':64,'h':8}),
    ('emu_vram_read', {'x':384,'y':256,'w':64,'h':8}),
    ('screenshot_file', {'path': 'duck_opt.png'}),
    ('screenshot', {'path': 'duck_opt.png'}),
    ('gpu_ring', {}),
    ('gp0_ring', {}),
]:
    r = call(4371, cmd, **kw)
    print(f"  {cmd} {kw}: {str(r)[:160]}")
