#!/usr/bin/env python3
"""Decode MIPS instructions from hex."""
import struct

hex_str = "020004240c0000000800e00300000000a0000a2408004001"
data = bytes.fromhex(hex_str)
addr = 0x8005AA10

for i in range(0, len(data), 4):
    w = struct.unpack('<I', data[i:i+4])[0]
    op = (w >> 26) & 0x3F
    rs = (w >> 21) & 0x1F
    rt = (w >> 16) & 0x1F
    rd = (w >> 11) & 0x1F
    imm = w & 0xFFFF
    simm = imm if imm < 0x8000 else imm - 0x10000
    target26 = (w & 0x03FFFFFF) << 2
    func = w & 0x3F

    pc = addr + i
    if op == 0x09:  # ADDIU
        print(f"  {pc:08X}: addiu ${rt}, ${rs}, {simm}  (0x{w:08X})")
    elif op == 0x0F:  # LUI
        print(f"  {pc:08X}: lui ${rt}, 0x{imm:04X}  (0x{w:08X})")
    elif op == 0x00 and func == 0x08:  # JR
        print(f"  {pc:08X}: jr ${rs}  (0x{w:08X})")
    elif op == 0x00 and func == 0x00 and w == 0:  # NOP
        print(f"  {pc:08X}: nop  (0x{w:08X})")
    elif op == 0x03:  # JAL
        jtarget = (pc & 0xF0000000) | target26
        print(f"  {pc:08X}: jal 0x{jtarget:08X}  (0x{w:08X})")
    elif op == 0x02:  # J
        jtarget = (pc & 0xF0000000) | target26
        print(f"  {pc:08X}: j 0x{jtarget:08X}  (0x{w:08X})")
    else:
        print(f"  {pc:08X}: ??? op={op} 0x{w:08X}")
