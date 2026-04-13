#!/usr/bin/env python3
"""Generate phase2_ghidra_seeds.json from Ghidra function starts + Phase 1c seeds + supplemental seeds.

This script MERGES sources. If phase2_ghidra_seeds.json already exists, any seeds
in it that don't come from the current Ghidra export are preserved (they were added
manually in prior sessions).
"""
import json
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

with open(os.path.join(root, "generated", "ghidra_function_starts.json")) as f:
    data = json.load(f)

seeds = []
existing = set()

# Load existing seed file to preserve manually-added seeds
out_path = os.path.join(root, "recompiler", "seeds", "phase2_ghidra_seeds.json")
old_seeds = {}
if os.path.exists(out_path):
    with open(out_path) as f:
        old_data = json.load(f)
    for s in old_data.get("seeds", []):
        addr_int = int(s["address"], 16)
        old_seeds[addr_int] = s

# Add the two original Phase 1c seeds first
for addr, label, rat in [
    ("0xBFC00000", "reset_vector", "MIPS R3000A reset vector"),
    ("0xBFC00180", "general_exception_bev1", "BEV=1 general exception vector"),
]:
    seeds.append({"address": addr, "label": label, "rationale": rat})
    existing.add(int(addr, 16))

# Add all Ghidra function starts
for addr_str in data["function_starts"]:
    addr_int = int(addr_str, 16)
    if addr_int not in existing:
        label = "ghidra_{:08X}".format(addr_int)
        seeds.append({"address": "0x{:08X}".format(addr_int), "label": label, "rationale": "Ghidra function start"})
        existing.add(addr_int)

# Add dispatch-miss seeds (functions only reachable via indirect calls)
dm_path = os.path.join(root, "recompiler", "seeds", "dispatch_miss_seeds.json")
if os.path.exists(dm_path):
    with open(dm_path) as f:
        dm_data = json.load(f)
    for s in dm_data.get("seeds", []):
        addr_int = int(s["address"], 16)
        if addr_int not in existing:
            label = "dmiss_{:08X}".format(addr_int)
            seeds.append({"address": "0x{:08X}".format(addr_int), "label": label, "rationale": s.get("rationale", "dispatch miss")})
            existing.add(addr_int)

# Preserve manually-added seeds from the previous file
preserved = 0
for addr_int, s in sorted(old_seeds.items()):
    if addr_int not in existing:
        seeds.append(s)
        existing.add(addr_int)
        preserved += 1

result = {
    "schema": "psxrecomp-v4 phase2 seeds",
    "source": "generated/ghidra_function_starts.json + phase1c seeds + dispatch_miss_seeds + preserved manual",
    "seed_count": len(seeds),
    "seeds": seeds,
    "excluded": [
        {"address": "0xBFC00100", "label": "utlb_miss_bev1",
         "rationale": "R3000 UTLB-miss vector; PS1 has no TLB; never executed"}
    ],
}

with open(out_path, "w") as f:
    json.dump(result, f, indent=2)

print("Written {} seeds to {} ({} preserved from previous)".format(len(seeds), out_path, preserved))
