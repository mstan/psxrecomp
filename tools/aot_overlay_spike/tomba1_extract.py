#!/usr/bin/env python3
"""Broadened STATIC overlay extractor for Tomba 1 — ALL code overlays, not just X*.BIN.

A code overlay is any disc file whose head is a {count:u32, ptr[count]:u32} export
table with the pointers landing in the 0x800E7000 overlay window (nulls allowed for
unused slots). This catches X0N.BIN (17), INFO.BIN, DSPSUB*.BIN, A00*.000, etc.
Each loads verbatim/position-fixed at region 0x800E7000 + offset 904; seeds come
from a prologue scan at base 0x800E7388. Deduped by content (AREA vs SYSTEM copies).
"""
import sys, os, json, base64, struct, binascii
sys.path.insert(0, r"F:\Projects\psxrecomp\psxrecomp\tools")
import extract_overlays as eo

CUE = r"F:\Projects\psxrecomp\TombaRecomp\tomba\tomba.cue"
OUT = r"C:\Users\Matthew\AppData\Local\Temp\claude\F--Projects-psxrecomp\58ec5aaf-6d7b-4e75-b72a-d255c28f5d96\scratchpad\overlay_captures_static2.json"
REGION_BASE = 0x800E7000
LOAD_OFFSET = 904
FILL = b'\x11' * LOAD_OFFSET

bp, raw = eo.parse_cue(CUE); disc = eo.DiscReader(bp, raw=raw)
files = list(eo.enumerate_files(disc))

def is_code_overlay(data):
    """True if head is {count, ptr[count]} with ptrs in the 0x800E7000 window
    (nulls allowed). Returns count or None."""
    if len(data) < 8: return None
    cnt = struct.unpack_from('<I', data, 0)[0]
    if not (1 <= cnt <= 4096): return None
    if 4 + cnt * 4 > len(data): return None
    ptrs = struct.unpack_from(f'<{cnt}I', data, 4)
    inrange = sum(1 for p in ptrs if 0x800E7000 <= p < 0x80200000 and (p & 3) == 0)
    nulls   = sum(1 for p in ptrs if p == 0)
    # accept if every non-null pointer is a valid in-window aligned address
    return cnt if (inrange + nulls == cnt and inrange >= 1) else None

def prologue_seeds(data, base):
    out = []
    for off in range(0, len(data) - 4, 4):
        w = struct.unpack_from('<I', data, off)[0]
        if (w >> 16) == 0x27BD and (w & 0x8000):
            out.append(base + off)
    return out

# detect + dedup by content
seen = {}
overlays = []
for p, l, s in files:
    if s < 8 or s > 2_000_000: continue
    try: d = disc.read_file_bytes(l, s)
    except: continue
    cnt = is_code_overlay(d)
    if cnt is None: continue
    crc = binascii.crc32(d) & 0xFFFFFFFF
    if crc in seen: continue
    seen[crc] = 1
    overlays.append((p, d))

records = []
for p, d in sorted(overlays, key=lambda x: x[0]):
    seeds = prologue_seeds(d, REGION_BASE + LOAD_OFFSET)
    region = FILL + d
    records.append({
        "schema": "psxrecomp overlay capture v2",
        "load_addr": f"0x{REGION_BASE:08X}",
        "size": len(region),
        "bytes_b64": base64.b64encode(region).decode('ascii'),
        "executed_pcs": [],
        "dispatch_entry_pcs": [f"0x{a:08X}" for a in seeds],
        "function_entry_pcs": [f"0x{a:08X}" for a in seeds],
        "seeds": [f"0x{a:08X}" for a in seeds],
        "_src": p,
    })
    print(f"  {p:28s} {len(d):7d}B seeds={len(seeds)}")

json.dump(records, open(OUT, 'w'), indent=1)
print(f"\nwrote {len(records)} code-overlay capture records -> {OUT}")
