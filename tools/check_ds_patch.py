"""Check that duckstation-qt.exe contains our TCP patch strings."""
import sys
p = sys.argv[1] if len(sys.argv) > 1 else 'duckstation/build/bin/duckstation-qt.exe'
with open(p, 'rb') as f:
    data = f.read()
markers = [b'PSXRecompDebug', b'pc_break', b'read_ram', b'wtrace', b'psxrecomp_debug_server']
for m in markers:
    print(f'{m.decode()}: {"YES" if m in data else "no"}')
