"""Parse psx_last_run_report.json — show: reason, cpu state, last 30
dispatch_ring entries, last 5 unknown_dispatch entries, last 30
dirty_block_log entries. Highlight the relevant ones for the
0xBFC09790 caller investigation."""
import json
with open(r'F:\Projects\psxrecomp-v4\psx_last_run_report.json') as f:
    r = json.load(f)

print(f"reason: {r.get('reason')}")
print(f"timestamp: {r.get('timestamp')}")
print(f"frame: {r.get('frame')}")
print(f"last_func_addr: {r.get('last_func_addr')}")
print(f"last_store_pc: {r.get('last_store_pc')}")

cpu = r.get('cpu')
if cpu:
    print(f"\nCPU state:")
    print(f"  pc={cpu.get('pc')} epc={cpu.get('epc')} sr={cpu.get('sr')} cause={cpu.get('cause')}")
    print(f"  hi={cpu.get('hi')} lo={cpu.get('lo')}")
    gpr = cpu.get('gpr', [])
    NAMES = ['$0','at','v0','v1','a0','a1','a2','a3','t0','t1','t2','t3','t4','t5','t6','t7',
             's0','s1','s2','s3','s4','s5','s6','s7','t8','t9','k0','k1','gp','sp','fp','ra']
    for i in range(0, 32, 4):
        line = '  '
        for j in range(4):
            line += f"{NAMES[i+j]:>3}={gpr[i+j]} "
        print(line)

# Dispatch tail — last 30
dt = r.get('dispatch_tail', {})
addrs = dt.get('addrs', [])
print(f"\nDispatch tail (last 30 of {dt.get('count')} returned, total seq {dt.get('total')}):")
for a in addrs[-30:]:
    print(f"  {a}")

# Unknown dispatch tail
ud = r.get('unknown_dispatch_tail', {})
print(f"\nUnknown-dispatch tail (last 5 of {ud.get('count')} returned, total seq {ud.get('total')}):")
for e in ud.get('entries', [])[-5:]:
    print(f"  seq={e['seq']} phys={e['phys']} ra={e['ra']} a0={e['a0']} a1={e['a1']} frame={e['frame']}")

# Dirty block tail (last 20)
db = r.get('dirty_block_tail', {})
print(f"\nDirty-block tail (last 20 of {db.get('count')} returned, total seq {db.get('total')}):")
for e in db.get('entries', [])[-20:]:
    print(f"  seq={e['seq']} target={e['target']} ra={e['ra']} a0={e['a0']} frame={e['frame']}")
