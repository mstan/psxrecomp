"""For each thread saved at sp=X, BIOS ChangeTh prolog wrote Tomba's caller-ra
to mem[sp+0x14]. Read that to find the actual ChangeTh JAL site in Tomba."""
import socket, json

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

# TCB A: save sp = 0x801FFF98 -> mem[0x801FFFAC] = Tomba's caller ra
# TCB B: save sp = 0x801FE3A8 -> mem[0x801FE3BC] = Tomba's caller ra
for label, sp in [('TCB A (0xA000E1F4)', 0x801FFF98), ('TCB B (0xA000E2B4)', 0x801FE3A8)]:
    print(f"=== {label} ===")
    # Stack frame around the saved ra
    r = call(4470, 'read_ram', addr=f'0x{sp:08X}', len=0x30)
    b = bytes.fromhex(r['hex'])
    for i in range(0, len(b), 4):
        w = int.from_bytes(b[i:i+4], 'little')
        annot = '  <-- saved ra (Tomba caller PC after JAL)' if i == 0x14 else ''
        print(f"  sp+0x{i:02X} ({sp+i:08X}): 0x{w:08X}{annot}")
    print()

# Also dump on DuckStation for comparison
print(f"=== DuckStation comparison ===")
# DuckStation's threads might have different sp values
# Read its TCB to get current sp from saved area
for tcb, label in [(0xA000E1F4, 'A'), (0xA000E2B4, 'B')]:
    save_area = tcb + 8
    r = call(4371, 'read_ram', addr=f'0x{save_area + 29*4:08X}', len=4)  # gpr[29] = sp
    sp = int.from_bytes(bytes.fromhex(r['hex']), 'little')
    print(f"  DUCK TCB {label} saved sp = 0x{sp:08X}")
    if sp:
        r2 = call(4371, 'read_ram', addr=f'0x{sp+0x14:08X}', len=4)
        ra = int.from_bytes(bytes.fromhex(r2['hex']), 'little')
        print(f"  DUCK TCB {label} mem[sp+0x14] = 0x{ra:08X} (Tomba caller PC)")
