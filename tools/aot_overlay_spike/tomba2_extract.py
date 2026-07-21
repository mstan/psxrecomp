#!/usr/bin/env python3
"""STATIC overlay extractor for Tomba 2 (SCUS-94454) — play-free.

Two producers, both verbatim/position-fixed (verified vs vault):
 1. MAIN.EXE — a standard PS-X EXE (load 0x80010000, 698K). Its body is the
    dominant overlay. The runtime keys by window-clamped region_start, splitting
    at the overlay floor (0x38000): below-floor -> region 0x80010000, above-floor
    -> region 0x80038000. We emit both.
 2. BIN/A*.BIN — {count:u32, ptr[count]:u32} header (like Tomba 1) loading in the
    0x80106000 region. Load base recovered play-free by delta-sweep: the base
    where the header's absolute pointers land on addiu-sp prologues.
Seeds: prologue scan at each region's load address.
"""
import sys, os, json, base64, struct, binascii
sys.path.insert(0, r"F:\Projects\psxrecomp\psxrecomp\tools")
import extract_overlays as eo

CUE = r"F:\Projects\psxrecomp\Tomba2Recomp\tomba2\Tomba! 2 - The Evil Swine Return (USA).cue"
OUT = r"C:\Users\Matthew\AppData\Local\Temp\claude\F--Projects-psxrecomp\58ec5aaf-6d7b-4e75-b72a-d255c28f5d96\scratchpad\t2_captures.json"
FLOOR = 0x38000          # overlay floor offset (text_end 0x38800 -> page 0x38)
MAIN_LOAD = 0x80010000

bp, raw = eo.parse_cue(CUE); disc = eo.DiscReader(bp, raw=raw)
files = list(eo.enumerate_files(disc))
def rdfull(name):
    for p,l,s in files:
        if p.upper()==name.upper() or p.upper().endswith('/'+name.upper()): return disc.read_file_bytes(l,s)
    return None

def prologues(data, base):
    return [base+off for off in range(0,len(data)-4,4)
            if (struct.unpack_from('<I',data,off)[0]>>16)==0x27BD
            and (struct.unpack_from('<I',data,off)[0]&0x8000)]

def rec(load_addr, data, seeds):
    return {"schema":"psxrecomp overlay capture v2","load_addr":f"0x{load_addr:08X}",
            "size":len(data),"bytes_b64":base64.b64encode(data).decode(),
            "executed_pcs":[],"dispatch_entry_pcs":[f"0x{a:08X}" for a in seeds],
            "function_entry_pcs":[f"0x{a:08X}" for a in seeds],
            "seeds":[f"0x{a:08X}" for a in seeds]}

records=[]

# --- Producer 1: MAIN.EXE, split at floor ---
main=rdfull('MAIN.EXE'); body=main[2048:]
# below-floor region 0x80010000 .. 0x80038000
lo_len=FLOOR-(MAIN_LOAD & 0xFFFFF if False else 0x10000)  # 0x38000-0x10000 = 0x28000
lo=body[:0x28000]
records.append(rec(0x80010000, lo, prologues(lo,0x80010000)))
# above-floor region 0x80038000 ..
hi=body[0x28000:]
records.append(rec(0x80038000, hi, prologues(hi,0x80038000)))
print(f"MAIN.EXE: below-floor {len(lo)}B @0x80010000 seeds={len(prologues(lo,0x80010000))}; "
      f"above-floor {len(hi)}B @0x80038000 seeds={len(prologues(hi,0x80038000))}")

# --- Producer 2: BIN/A*.BIN via delta-sweep base recovery ---
def recover_base(data):
    cnt=struct.unpack_from('<I',data,0)[0]
    if not (1<=cnt<=4096) or 4+cnt*4>len(data): return None
    ptrs=[p for p in struct.unpack_from(f'<{cnt}I',data,4) if p]
    if not ptrs: return None
    # content starts after header; try candidate bases so header ptrs hit prologues
    hdr=4+cnt*4
    best=None
    lo=min(ptrs)&~0xFFF
    for base in range(lo-0x4000, lo+0x1000, 4):
        hits=0
        for p in ptrs:
            off=p-base
            if 0<=off<=len(data)-4:
                w=struct.unpack_from('<I',data,off)[0]
                if (w>>16)==0x27BD and (w&0x8000): hits+=1
        if best is None or hits>best[1]: best=(base,hits)
    return best  # (base, prologue-hits)

nbin=0
for p,l,s in sorted(files):
    if not (p.upper().startswith('BIN/') and p.upper().endswith('.BIN')): continue
    d=disc.read_file_bytes(l,s)
    r=recover_base(d)
    if not r or r[1]==0:
        print(f"  {p}: no base recovered (hits=0) — skip"); continue
    base,hits=r
    seeds=prologues(d,base)
    records.append(rec(base,d,seeds))
    nbin+=1
    if nbin<=6: print(f"  {p}: base=0x{base:08X} hdr_prologue_hits={hits} seeds={len(seeds)} size={s}")
print(f"BIN overlays: {nbin} extracted")

json.dump(records, open(OUT,'w'))
print(f"\nwrote {len(records)} records -> {OUT}")
