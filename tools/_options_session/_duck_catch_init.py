"""Reset DuckStation, arm mem_break on 0x8009B3A0, let it boot, capture the
PC that writes there. That's the init function we need to check on runtime."""
import socket, json, time

def call(cmd, **kw):
    s = socket.create_connection(('127.0.0.1', 4371), timeout=10)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.decode())

print("--- Reset DuckStation ---")
print(call('system_reset'))

# Wait for it to come back up
time.sleep(2)
print("ping:", call('ping'))

# Verify it really reset
print("mem[0x8009B3A0] after reset:", call('read_ram', addr='0x8009B3A0', len=4))

# Arm mem_break on the struct head
print()
print("--- Arm mem_break ---")
print("mem_hit_clear:", call('mem_hit_clear'))
print("mem_break:    ", call('mem_break', addr='0x8009B3A0'))

# Poll for the write — write happens early in boot
print()
print("--- Polling for write (60 sec max) ---")
end = time.time() + 60
caught = []
while time.time() < end:
    h = call('mem_hit_last')
    if h.get('valid'):
        key = (h.get('pc'), h.get('ra'), h.get('mem_addr'))
        if key not in caught:
            caught.append(key)
            p = call('ping')
            v = call('read_ram', addr='0x8009B3A0', len=4)
            print(f"  fr={p.get('frame','?')} pc={h.get('pc')} ra={h.get('ra')} mem_addr={h.get('mem_addr')} val_now={v.get('hex')}")
        call('mem_hit_clear')
    # Also peek at value periodically
    time.sleep(0.05)

if not caught:
    print("  No writes caught in 60 sec. Value now:", call('read_ram', addr='0x8009B3A0', len=4))
print()
print(f"Total distinct writes: {len(caught)}")
