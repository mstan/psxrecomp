"""Track display_x/y over time on runtime + duck.
If double-buffering is working, display origin should toggle between
two values every frame (or every other frame)."""
import socket, json, time

def call(port, cmd, **kw):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    s.settimeout(2)
    buf = b''
    out = None
    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        buf += chunk
        while b'\n' in buf:
            line, _, buf = buf.partition(b'\n')
            try:
                obj = json.loads(line.decode())
            except Exception:
                continue
            if obj.get('id') == 1:
                out = obj
                deadline = 0
                break
    s.close()
    return out or {'err': 'no response'}

for _ in range(3):
    call(4371, 'mem_hit_clear')

print("Sample display origin + draw offset 12 times over 2s:")
for i in range(12):
    rt = call(4470, 'gpu_state')
    dk = call(4371, 'gpu_state')
    rt_disp = f"({rt.get('display_x')},{rt.get('display_y')})"
    rt_draw = f"({rt.get('draw_offset',[None,None])[0]},{rt.get('draw_offset',[None,None])[1]})"
    dk_disp = f"({dk.get('display_area_x')},{dk.get('display_area_y')})"
    dk_draw = f"({dk.get('draw_offset_x')},{dk.get('draw_offset_y')})"
    rt_stat = rt.get('gpustat')
    dk_stat = dk.get('gpustat')
    print(f" [{i:2}] runtime: disp={rt_disp} draw={rt_draw} stat={rt_stat} | "
          f"duck: disp={dk_disp} draw={dk_draw} stat={dk_stat}")
    time.sleep(0.16)
