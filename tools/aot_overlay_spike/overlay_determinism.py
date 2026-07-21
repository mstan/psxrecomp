#!/usr/bin/env python3
"""Corpus analysis: is there a DETERMINISTIC story across captured overlays?

Scans every overlay_captures.json + cache/*.ranges under the project tree,
one representative per game, and quantifies:
  - base-address regularity (how many distinct load_addr per game; reuse depth)
  - relation of each base to the EXE text layout (load_address, text_size from game.toml)
  - size distribution + sector (2048) alignment
  - seed / dispatch-entry clustering (offset of entries from the region base)
  - cross-variant function-CRC stability (from .ranges: do (entry,crc) recur?)
"""
import json, os, re, glob, base64
from collections import defaultdict, Counter

ROOT = r"F:\Projects\psxrecomp"

# ---- pick one representative captures.json per game id (prefer vault, else largest) ----
def game_id_of(path):
    m = re.search(r'(SC[UE]S-\d{5}|SLP[SM]-\d{5})', path)
    if m: return m.group(1)
    # fall back to repo dir name
    parts = path.replace('\\','/').split('/')
    for p in parts:
        if p.endswith('Recomp'): return p
    return parts[-2]

def rec_count(p):
    try:
        with open(p, 'r', errors='ignore') as f: return f.read().count('"load_addr"')
    except: return 0

cands = defaultdict(list)
for p in glob.glob(os.path.join(ROOT, '**', 'overlay_captures.json'), recursive=True):
    if any(x in p for x in ('_wt-', '_stale', '_backup', '_release_test')): continue
    cands[game_id_of(p)].append(p)

reps = {}
for gid, ps in cands.items():
    ps.sort(key=lambda p: (0 if '_coverage_vault' in p else 1, -rec_count(p)))
    reps[gid] = ps[0]

# ---- load game.toml text layout per game ----
def find_toml_layout(gid):
    for tp in glob.glob(os.path.join(ROOT, '*Recomp*', 'game.toml')):
        try:
            t = open(tp, errors='ignore').read()
        except: continue
        if gid in t or gid.replace('-','_') in t:
            la = re.search(r'load_address\s*=\s*"?(0x[0-9A-Fa-f]+)"?', t)
            ts = re.search(r'text_size\s*=\s*"?(0x[0-9A-Fa-f]+)"?', t)
            return (int(la.group(1),16) if la else None,
                    int(ts.group(1),16) if ts else None, tp)
    return (None, None, None)

def norm(a):  # KSEG-mask to physical, keep 0x80000000 base for display
    return a & 0x1FFFFFFF

print("="*78)
print("OVERLAY DETERMINISM CORPUS ANALYSIS")
print("="*78)

for gid in sorted(reps, key=lambda g: -rec_count(reps[g])):
    path = reps[gid]
    try:
        data = json.load(open(path, errors='ignore'))
    except Exception as e:
        print(f"\n## {gid}: FAILED to parse ({e})"); continue
    if not isinstance(data, list) or not data: continue
    la, ts, tp = find_toml_layout(gid)
    text_end = (la + ts) if (la and ts) else None

    by_base = defaultdict(list)
    for r in data:
        try: base = int(r['load_addr'], 16)
        except: continue
        by_base[base].append(r)

    print(f"\n{'#'*70}\n## {gid}   ({len(data)} records, {len(by_base)} distinct load_addr)")
    print(f"   repr: {os.path.relpath(path, ROOT)}")
    if la: print(f"   EXE: load_address={hex(la)} text_size={hex(ts) if ts else '?'} "
                 f"text_end={hex(text_end) if text_end else '?'}")

    for base in sorted(by_base):
        recs = by_base[base]
        sizes = [r.get('size',0) for r in recs]
        uq = sorted(set(sizes))
        align = Counter('sector' if s % 2048 == 0 else ('1k' if s % 1024 == 0 else 'other') for s in sizes)
        # entry offsets: first seed/dispatch relative to base
        offs = []
        for r in recs:
            seeds = r.get('seeds') or r.get('dispatch_entry_pcs') or []
            for s in seeds:
                try: offs.append(int(s,16) - base)
                except: pass
        rel = ""
        if text_end is not None:
            if norm(base) == norm(text_end): rel = "  <== EXACTLY text_end"
            elif abs(norm(base)-norm(text_end)) < 0x2000: rel = f"  (~text_end {norm(base)-norm(text_end):+#x})"
            elif norm(base) < norm(la): rel = "  (below load_address / low-RAM)"
        print(f"   base {hex(base)}: {len(recs)} caps{rel}")
        print(f"       sizes: {len(uq)} unique, min={min(sizes)} max={max(sizes)} "
              f"({min(sizes)//1024}K..{max(sizes)//1024}K), align={dict(align)}")
        if offs:
            offs_s = sorted(set(offs))
            head = [hex(o) for o in offs_s[:6]]
            print(f"       entry offsets from base: {len(offs_s)} unique, "
                  f"min={hex(min(offs))} max={hex(max(offs))}, first few={head}")

# ---- .ranges cross-variant function CRC stability ----
print(f"\n{'='*78}\nFUNCTION-CRC STABILITY (from compiled .ranges shards)\n{'='*78}")
for gid in sorted(reps):
    vault = os.path.join(ROOT, '_coverage_vault', gid, 'cache')
    ranges = glob.glob(os.path.join(vault, '**', '*.ranges'), recursive=True)
    if not ranges: continue
    # group by region (phys prefix in filename), collect (entry->set(crc))
    region_funcs = defaultdict(lambda: defaultdict(set))
    for rf in ranges:
        stem = os.path.basename(rf)
        region = stem.split('_')[0]
        for line in open(rf, errors='ignore'):
            m = re.match(r'F ([0-9A-Fa-f]+) ([0-9A-Fa-f]+)', line)
            if m: region_funcs[region][m.group(1)].add(m.group(2))
    print(f"\n## {gid}: {len(ranges)} .ranges files")
    for region, funcs in sorted(region_funcs.items()):
        stable = sum(1 for e,cs in funcs.items() if len(cs)==1)
        variable = sum(1 for e,cs in funcs.items() if len(cs)>1)
        print(f"   region {region}: {len(funcs)} distinct entry PCs; "
              f"{stable} stable(1 crc), {variable} multi-crc "
              f"({100*stable//max(1,len(funcs))}% stable-address)")
