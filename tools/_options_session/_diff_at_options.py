"""Compare runtime (4470) vs DuckStation (4371) at OPTIONS.
Handles scratchpad correctly on both sides.
Samples values over 3 seconds to spot static-vs-dynamic differences."""
import socket, json, time, struct, sys

def call(port, cmd, **kw):
    try:
        s = socket.create_connection(('127.0.0.1', port), timeout=3)
        s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
        buf = b''
        while not buf.endswith(b'\n'):
            c = s.recv(65536)
            if not c: break
            buf += c
        s.close()
        return json.loads(buf.decode())
    except Exception as e:
        return {'error': str(e)}

def read_word(port, addr):
    """Read a 32-bit LE word, handling scratchpad correctly on DuckStation."""
    is_scratchpad = (addr & 0xFFFFE000) == 0x1F800000
    if port == 4371 and is_scratchpad:
        r = call(port, 'read_scratch', addr=f'0x{(addr & 0x3FF):04X}', len=4)
    else:
        r = call(port, 'read_ram', addr=f'0x{addr:08X}', len=4)
    if 'error' in r or not r.get('ok'):
        return None
    return int.from_bytes(bytes.fromhex(r['hex']), 'little')

def ping(port):
    return call(port, 'ping')

# Verify both alive
rt = ping(4470)
duck = ping(4371)
print(f"runtime:     {rt}")
print(f"duckstation: {duck}")
print()

if 'error' in rt or 'error' in duck:
    print("Both backends must be running & at OPTIONS. Re-launch any that errored.")
    sys.exit(1)

# Static comparison — values that should NOT change frame to frame
print("=== STATIC values (these should match if both at same state) ===")
print(f"{'addr':>14}  {'runtime':>12}  {'duckstation':>12}  {'match':>6}")
for addr, label in [
    (0x1F8001D4, "scratchpad[0x1D4] state struct ptr"),
    (0x80097520, "fn ptr table+0"),
    (0x80097524, "fn ptr table+4 (state-0 helper)"),
    (0x80097528, "fn ptr table+8"),
    (0x8009752C, "fn ptr table+C"),
    (0x80000108, "kernel TCB list ptr"),
    (0x80000110, "kernel TCBH"),
    (0x80000120, "kernel EvCB ptr"),
]:
    rt_v = read_word(4470, addr)
    du_v = read_word(4371, addr)
    if rt_v is None or du_v is None:
        print(f"  0x{addr:08X}  err  err  ?  {label}")
        continue
    m = "OK" if rt_v == du_v else "DIFF"
    print(f"  0x{addr:08X}  0x{rt_v:08X}  0x{du_v:08X}  {m:>6}  {label}")
print()

# Dynamic comparison — sample 10 times, see which values CHANGE
print("=== DYNAMIC values (sample 10 frames; see who's animating) ===")
addrs = [
    (0x8009B3D4, "state-0 helper struct +0x34"),
    (0x8009B3A0, "state-0 helper struct base"),
]

for addr, label in addrs:
    print(f"\n  Address 0x{addr:08X} ({label}):")
    rt_vals = []
    du_vals = []
    for i in range(10):
        rt_vals.append(read_word(4470, addr))
        du_vals.append(read_word(4371, addr))
        time.sleep(0.05)
    rt_unique = len(set(rt_vals))
    du_unique = len(set(du_vals))
    print(f"    runtime values: {[hex(v) if v is not None else '?' for v in rt_vals]}  ({rt_unique} unique --> {'FROZEN' if rt_unique == 1 else 'animating'})")
    print(f"    duck    values: {[hex(v) if v is not None else '?' for v in du_vals]}  ({du_unique} unique --> {'FROZEN' if du_unique == 1 else 'animating'})")
