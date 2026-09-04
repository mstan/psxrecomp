"""gte_audit — audit COP2/GTE instruction coverage.

Generic across any PSX program. For each declared code region in the
config, scans the ROM for COP2 (GTE command) and LWC2/SWC2 (GTE
load/store) instructions and confirms each one has a matching emit in
the generated C file.

Migration note: the original BIOS-only version scanned the entire ROM
including data regions, producing false positives in `0xBFC75xxx+`.
This version filters to declared code regions only — which closes the
data-region false-positive bug class noted in `docs/internal/audit_inventory.md`.

Usage:
  python tools/gte_audit.py --config bios/SCPH1001.toml
  python tools/gte_audit.py --config /path/to/game.toml

Exit code:
  0  no missing GTE emits
  1  at least one missing GTE / LWC2 / SWC2 emit
"""
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import audit_config


def main():
    cfg = audit_config.from_argv(sys.argv)
    print(f"# gte audit: {cfg.name}")
    print(f"  rom    = {cfg.rom}")
    print(f"  full_c = {cfg.full_c}")
    print()

    if not cfg.rom.exists():
        print(f"ERROR: rom not found: {cfg.rom}", file=sys.stderr)
        return 2
    if not cfg.full_c.exists():
        print(f"ERROR: full_c not found: {cfg.full_c}", file=sys.stderr)
        return 2

    rom_bytes = cfg.rom.read_bytes()

    # ── 1. Scan ROM code regions for COP2 commands, LWC2, SWC2 ─────────────

    rom_gte = []          # (vaddr, word)
    rom_lwc2_swc2 = []    # (vaddr, word)

    for region in cfg.regions:
        if region.rom_end > len(rom_bytes):
            print(f"WARNING: region {region.name} extends past ROM file "
                  f"(rom_end=0x{region.rom_end:X} > size=0x{len(rom_bytes):X})",
                  file=sys.stderr)
            continue
        for off in range(region.rom_start, region.rom_end, 4):
            word = struct.unpack_from("<I", rom_bytes, off)[0]
            opcode = (word >> 26) & 0x3F
            vaddr = region.vaddr_base + (off - region.rom_start)

            if opcode == 0x12:  # COP2
                bit25 = (word >> 25) & 1
                if bit25 == 1:  # GTE command (not MFC2/MTC2/CFC2/CTC2)
                    rom_gte.append((vaddr, word))
            elif opcode in (0x32, 0x3A):  # LWC2 or SWC2
                rom_lwc2_swc2.append((vaddr, word))

    # ── 2. Scan generated C for matching emits ─────────────────────────────

    gen_gte_addrs = set()
    gen_cop2_load_addrs = set()

    # Address comment pattern: /* 0xXXXXXXXX: ...
    addr_re = re.compile(r'/\* 0x([0-9A-Fa-f]{8}):')

    with open(cfg.full_c, "r", encoding="utf-8", errors="replace") as f:
        prev_addr = None
        for line in f:
            m = addr_re.search(line)
            if m:
                prev_addr = int(m.group(1), 16)
            if "gte_execute" in line and prev_addr is not None:
                gen_gte_addrs.add(prev_addr)
            if ("lwc2" in line.lower() or "swc2" in line.lower()) and prev_addr is not None:
                if m:  # this line has the address comment
                    gen_cop2_load_addrs.add(prev_addr)

    # ── 3. Diff and report ─────────────────────────────────────────────────

    print(f"ROM code regions scanned       : {len(cfg.regions)}")
    print(f"ROM GTE commands (COP2 bit25=1): {len(rom_gte)}")
    print(f"ROM LWC2/SWC2 instructions     : {len(rom_lwc2_swc2)}")
    print(f"Generated gte_execute calls    : {len(gen_gte_addrs)}")
    print(f"Generated lwc2/swc2 accesses   : {len(gen_cop2_load_addrs)}")
    print()

    missing_gte = [(addr, word) for addr, word in rom_gte
                   if addr not in gen_gte_addrs]
    missing_loads = [(addr, word) for addr, word in rom_lwc2_swc2
                     if addr not in gen_cop2_load_addrs]

    if missing_gte:
        print(f"MISSING GTE commands: {len(missing_gte)}")
        regions = defaultdict(list)
        for addr, word in missing_gte:
            regions[addr & ~0xFF].append((addr, word, word & 0x3F))
        print(f"Missing spread across {len(regions)} 256B clusters:")
        for region in sorted(regions):
            entries = regions[region]
            codes = ", ".join(f"0x{e[2]:02X}" for e in entries)
            print(f"  0x{region:08X}: {len(entries)} missing — funct: {codes}")
            for a, w, c in entries[:5]:
                print(f"    0x{a:08X}: 0x{w:08X} (funct {c:#04x})")
            if len(entries) > 5:
                print(f"    ... and {len(entries) - 5} more")
    else:
        print("MISSING GTE commands: 0 OK")

    print()
    if missing_loads:
        print(f"MISSING LWC2/SWC2: {len(missing_loads)} of {len(rom_lwc2_swc2)}")
        load_regions = defaultdict(list)
        for addr, word in missing_loads:
            load_regions[addr & ~0xFF].append(addr)
        for region in sorted(load_regions):
            print(f"  0x{region:08X}: {len(load_regions[region])} missing")
    else:
        print(f"MISSING LWC2/SWC2: 0 OK (of {len(rom_lwc2_swc2)} total)")

    print()
    real_misses = len(missing_gte) + len(missing_loads)
    print(f"[summary] {cfg.name}")
    print(f"  missing gte_execute emits : {len(missing_gte)}")
    print(f"  missing lwc2/swc2 emits   : {len(missing_loads)}")
    print(f"  total real-bug findings   : {real_misses}")
    print(f"  STATUS                    : {'CLEAN' if real_misses == 0 else 'ISSUES FOUND'}")

    return 0 if real_misses == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
