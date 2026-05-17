"""Pull thread_ctx_ring from runtime at OPTIONS.
For each TCB, show the sequence of save -> restore -> save -> restore.
The 'resume_pc' field should EVOLVE if the thread does real work between yields.
If it's static (same PC every save), the thread is stuck."""
import socket, json, collections

def call(cmd, **kw):
    s = socket.create_connection(('127.0.0.1', 4470), timeout=5)
    s.sendall((json.dumps({'id':1,'cmd':cmd, **kw})+'\n').encode())
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(65536)
        if not c: break
        buf += c
    s.close()
    return json.loads(buf.decode())

r = call('thread_ctx_ring', count=256)
es = r.get('entries', [])
print(f"Got {len(es)} entries (total={r.get('total')}, available={r.get('available')})")
print()

# Group by TCB
per_tcb = collections.defaultdict(list)
for e in es:
    per_tcb[e['tcb']].append(e)

for tcb, entries in per_tcb.items():
    print(f"=== TCB {tcb} ({len(entries)} entries) ===")
    saves = [e for e in entries if e['op'] == 'save']
    restores = [e for e in entries if e['op'] == 'restore']
    save_pcs = {e['resume_pc'] for e in saves}
    restore_pcs = {e['resume_pc'] for e in restores}
    print(f"  saves: {len(saves)}; distinct resume_pc: {len(save_pcs)} --> {save_pcs}")
    print(f"  restores: {len(restores)}; distinct resume_pc: {len(restore_pcs)} --> {restore_pcs}")

    # also distinct sp / ra
    save_sps = {e['sp'] for e in saves}
    save_ras = {e['ra'] for e in saves}
    print(f"  save SPs: {save_sps}")
    print(f"  save RAs: {save_ras}")
    print()

    # Show last 10 entries chronologically
    print(f"  Last 8 entries:")
    for e in entries[-8:]:
        print(f"    seq={e['seq']:5} fr={e['frame']:5} {e['op']:>7} resume_pc={e['resume_pc']} sp={e['sp']} ra={e['ra']} sr={e['sr']} epc={e['epc']}")
    print()
