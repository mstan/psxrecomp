#!/usr/bin/env python3
"""Decode a jump table from hex dump."""
import sys

hex_str = "e0620380bc6303809863038044630380bc630380bc630380bc63038084630380d0620380b8620380bc6303801c630380bc63038008620380bc6303802c630380606203808c62038018620380bc630380586303807463038008630380ac630380"
data = bytes.fromhex(hex_str)

for i in range(24):
    off = i * 4
    val = int.from_bytes(data[off:off+4], 'little')
    cmd = i + 2  # command code = index + 2
    # Convert KSEG0 RAM addr to ROM addr: RAM 0x30000+ -> ROM 0x1FC18000+
    phys = val & 0x1FFFFFFF
    rom = 0xBFC00000 + (phys - 0x30000 + 0x18000) if 0x30000 <= phys <= 0x5AFFF else phys
    print(f"  [{i:2d}] cmd=0x{cmd:02X}  target=0x{val:08X}  ROM=0x{rom:08X}")
