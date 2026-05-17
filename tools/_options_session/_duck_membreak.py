"""Check if DuckStation already captured a write to 0x8009B3D4.
Arm mem_break, then query mem_hit_last."""
import socket, json, sys

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

# Address to watch
ADDR = 0x8009B3D4

print(f"mem_hit_clear: {call('mem_hit_clear')}")
print(f"mem_break addr=0x{ADDR:08X}: {call('mem_break', addr=f'0x{ADDR:08X}')}")
print()
print("Sleeping 2 sec to see if anything writes...")
import time
time.sleep(2)
print(f"mem_hit_last: {call('mem_hit_last')}")
print()
print("Current memory value:")
r = call('read_ram', addr=f'0x{ADDR:08X}', len=4)
print(f"  mem[0x{ADDR:08X}] = {r.get('hex')}  (oracle SHOULD be 00010000 LE = 0x00000100)")
