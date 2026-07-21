#!/usr/bin/env python3
"""Extract a PS-X EXE from a PlayStation BIN/CUE disc image.

Usage:
    python3 tools/extract_psx_exe.py <image.bin> [output.exe]

If output.exe is omitted, the basename of the input is used with .exe extension.
"""

import struct
import sys
import os

# Sync pattern for CD-ROM sectors
CD_SYNC = b'\x00' * 12
SECTOR_SIZE = 2352
SYNC_HEADER_SIZE = 24  # 12 sync + 4 header + 8 subheader (Mode 2)
DATA_SIZE = 2048

PSX_EXE_MAGIC = b'PS-X EXE'


def read_sectors(path):
    """Read raw CD sectors and yield 2048-byte data chunks."""
    with open(path, 'rb') as f:
        while True:
            sector = f.read(SECTOR_SIZE)
            if len(sector) < SECTOR_SIZE:
                break
            # Mode 2 / CD-ROM XA: skip sync(12) + header(4) + subheader(8)
            data = sector[SYNC_HEADER_SIZE:SYNC_HEADER_SIZE + DATA_SIZE]
            yield data


ISO9660_SECTOR = 16  # First volume descriptor starts at sector 16


def parse_iso9660_directory(sectors, extent_sector, extent_size):
    """Parse an ISO 9660 directory record to find files.
    
    Yields (filename, extent_sector, extent_size, flags) for each file entry.
    Filenames use the ISO 9660 format (e.g. 'SYSTEM.CNF;1').
    """
    buf = bytearray()
    remaining = extent_size
    cur_sec = extent_sector
    while remaining > 0 and cur_sec < len(sectors):
        take = min(DATA_SIZE, remaining)
        buf.extend(sectors[cur_sec][:take])
        remaining -= take
        cur_sec += 1

    pos = 0
    while pos < len(buf):
        entry_len = buf[pos]
        if entry_len == 0:
            pos += 1  # Padding
            # Skip to next aligned sector boundary if needed
            if pos % DATA_SIZE == 0:
                continue
            continue
        if entry_len < 10 or pos + entry_len > len(buf):
            break
        
        # Directory record:
        # 0: entry_len
        # 1: ext_attr_len
        # 2-9: extent location (u32 both-byte orders)
        # 10-17: data length (u32 both-byte orders)
        # 18-24: recording date/time
        # 25: flags
        # 26: file unit size
        # 27: interleave gap size
        # 28-31: volume sequence number (u16 both-byte orders)
        # 32: file identifier length
        # 33+: file identifier
        ext_loc = struct.unpack_from('<I', buf, pos + 2)[0]
        data_len = struct.unpack_from('<I', buf, pos + 10)[0]
        flags = buf[pos + 25]
        name_len = buf[pos + 32]
        name = buf[pos + 33:pos + 33 + name_len].decode('ascii', errors='replace').rstrip(';').strip()
        yield (name, ext_loc, data_len, flags)
        pos += entry_len


def find_system_cnf(sectors):
    """Find and read SYSTEM.CNF from the ISO 9660 filesystem.
    
    Returns (content as string, game_id extracted from boot= line) or None.
    """
    if 16 >= len(sectors):
        return None
    
    # Parse primary volume descriptor at sector 16
    vd = sectors[16]
    
    # Volume descriptor at offset 0:
    # 0: descriptor type (1 = primary)
    # 1-5: 'CD001'
    # 6: version (1)
    # 156-190: volume identifier
    # 156+2 (offset 158): root directory record
    #   root_dir_extent at record offset 2 (within the record)
    #   root_dir_size at record offset 10
    
    # Root directory record starts at offset 156 (0x9C) in the VD
    root_rec_start = 156
    root_ext_loc = struct.unpack_from('<I', vd, root_rec_start + 2)[0]
    root_ext_size = struct.unpack_from('<I', vd, root_rec_start + 10)[0]
    
    # Scan the root directory for SYSTEM.CNF
    for name, ext_loc, data_len, flags in parse_iso9660_directory(sectors, root_ext_loc, root_ext_size):
        if name.upper() in ('SYSTEM.CNF', 'SYSTEM.CNF;1'):
            # Read its content
            content_bytes = bytearray()
            remaining = data_len
            cur_sec = ext_loc
            while remaining > 0 and cur_sec < len(sectors):
                take = min(DATA_SIZE, remaining)
                content_bytes.extend(sectors[cur_sec][:take])
                remaining -= take
                cur_sec += 1
            content = content_bytes.decode('ascii', errors='replace').strip()
            
            # Extract game ID from boot = cdrom:\XXX;1
            game_id = None
            for line in content.splitlines():
                line = line.strip()
                if line.upper().startswith('BOOT'):
                    # boot = cdrom:\FILENAME.EXT;1
                    if '=' in line:
                        val = line.split('=', 1)[1].strip()
                        # Strip cdrom:\ prefix and ;1 suffix
                        if val.upper().startswith('CDROM:\\\\') or val.upper().startswith('CDROM:\\'):
                            val = val.split('\\', 1)[1] if '\\' in val else val.split('/', 1)[1]
                        val = val.rstrip(';').strip()
                        game_id = val
            return content, game_id
    
    return None


def parse_exe_header(data):
    """Parse PS-X EXE header from the first sector of the EXE."""
    if data[:8] != PSX_EXE_MAGIC:
        print(f"  ERROR: not a PS-X EXE (magic: {data[:8]!r})")
        return None

    # PS-X EXE header layout:
    # 0x00: magic "PS-X EXE" (8)
    # 0x08: reserved (8)
    # 0x10: initial_pc  (u32 le)
    # 0x14: initial_gp  (u32 le)
    # 0x18: load_address (u32 le)
    # 0x1C: file_size    (u32 le) — size of EXE excluding header
    # 0x20: first_sector  (u16 le) — not the sector on disc but the cell/sector offset in the file area
    # 0x22: reserved (u16)
    # 0x24: load_address2 (u32 le) — same as 0x18 for standard EXEs
    # 0x28: reserved (32)
    # 0x38: memfill_start (u32 le)
    # 0x3C: memfill_size  (u32 le)
    # 0x40: initial_sp_base (u32 le) — initial $sp
    # 0x44: initial_sp_offset (u32 le) — $sp = sp_base - sp_offset
    # 0x48: reserved (58)
    # 0x82: region string (13)
    # 0x8F: reserved (up to 0x800)

    magic    = data[0:8]
    initial_pc  = struct.unpack_from('<I', data, 0x10)[0]
    initial_gp  = struct.unpack_from('<I', data, 0x14)[0]
    load_addr   = struct.unpack_from('<I', data, 0x18)[0]
    file_size   = struct.unpack_from('<I', data, 0x1C)[0]
    memfill_start = struct.unpack_from('<I', data, 0x38)[0]
    memfill_size  = struct.unpack_from('<I', data, 0x3C)[0]
    sp_base    = struct.unpack_from('<I', data, 0x40)[0]
    sp_offset  = struct.unpack_from('<I', data, 0x44)[0]
    region     = data[0x82:0x82+13].rstrip(b'\x00').decode('ascii', errors='replace')

    return {
        'magic': magic,
        'initial_pc': initial_pc,
        'initial_gp': initial_gp,
        'load_address': load_addr,
        'file_size': file_size,
        'memfill_start': memfill_start,
        'memfill_size': memfill_size,
        'sp_base': sp_base,
        'sp_offset': sp_offset,
        'region': region,
    }


def find_exe(sectors):
    """Scan data sectors for PS-X EXE magic, return (sector_offset, header)."""
    for i, data in enumerate(sectors):
        if data[:8] == PSX_EXE_MAGIC:
            header = parse_exe_header(data)
            if header:
                return i, header
    return None, None


def extract_exe(bin_path, output_path):
    """Extract PS-X EXE from a BIN/CUE disc image."""
    print(f"Scanning {bin_path} for PS-X EXE...")

    sectors = list(read_sectors(bin_path))
    print(f"  Read {len(sectors)} data sectors ({len(sectors) * DATA_SIZE / 1024 / 1024:.1f} MB)")

    # Try to find SYSTEM.CNF and game ID
    cnf_result = find_system_cnf(sectors)
    game_id = None
    if cnf_result:
        cnf_content, game_id = cnf_result
        print(f"\n  SYSTEM.CNF content:")
        for line in cnf_content.splitlines():
            print(f"    {line}")
        if game_id:
            print(f"\n  Game ID (from SYSTEM.CNF boot=): {game_id}")
        else:
            print(f"\n  Game ID: could not determine from SYSTEM.CNF")

    sector_off, header = find_exe(sectors)
    if not header:
        print("  ERROR: PS-X EXE not found on disc.")
        sys.exit(1)

    print(f"\n  Found EXE at data sector {sector_off}")
    print(f"    Magic:       {header['magic'].decode()}")
    print(f"    Load addr:   0x{header['load_address']:08X}")
    print(f"    File size:   {header['file_size']} bytes ({header['file_size'] / 1024:.1f} KB)")
    print(f"    Initial PC:  0x{header['initial_pc']:08X}")
    print(f"    Initial GP:  0x{header['initial_gp']:08X}")
    print(f"    SP base:     0x{header['sp_base']:08X}")
    print(f"    SP offset:   0x{header['sp_offset']:08X}", end='')
    sp_final = header['sp_base'] - header['sp_offset']
    if header['sp_base'] != 0:
        print(f"  → SP = 0x{sp_final:08X}")
    else:
        print("  [not set in EXE header]")
    print(f"    Memfill:     0x{header['memfill_start']:08X}  size={header['memfill_size']} bytes")
    print(f"    Region:      {header['region']}")

    # Calculate how many sectors the EXE spans (header is 0x800 bytes = 1 sector,
    # then file_size bytes of code/data)
    total_exe_size = 0x800 + header['file_size']
    num_sectors = (total_exe_size + DATA_SIZE - 1) // DATA_SIZE

    print(f"    Total EXE:   {total_exe_size} bytes spanning {num_sectors} sectors")

    # Extract
    out_data = bytearray()
    for i in range(num_sectors):
        idx = sector_off + i
        if idx < len(sectors):
            out_data.extend(sectors[idx])
        else:
            print(f"  WARNING: sector {idx} beyond disc end, padding with zeros")
            out_data.extend(b'\x00' * DATA_SIZE)

    with open(output_path, 'wb') as f:
        f.write(out_data)

    actual_size = os.path.getsize(output_path)
    print(f"\n  Written: {output_path} ({actual_size} bytes)")

    # Determine game ID
    if game_id:
        # Convert exe filename to a canonical ID for toml
        # e.g. SLUS_000.42;1 → SLUS-00042
        display_id = game_id.replace(';1', '').replace('.', '').replace('_', '-').replace(';', '')
    else:
        display_id = header['region']

    # Print TOML snippet for game.toml
    print(f"\n  ── game.toml [game] block ──")
    print(f"  [game]")
    print(f"  name          = \"Gex\"")
    print(f"  id            = \"{display_id}\"")
    print(f"  exe           = \"{output_path}\"")
    print(f"  load_address  = \"0x{header['load_address']:08X}\"")
    print(f"  entry_pc      = \"0x{header['initial_pc']:08X}\"")
    print(f"  text_size     = \"0x{header['file_size']:08X}\"")
    if header['sp_base'] != 0:
        print(f"  stack_base    = \"0x{sp_final:08X}\"")
    else:
        # Check SYSTEM.CNF for STACK= value
        stack_from_cnf = "0x801FFF00"  # Common default
        if cnf_result:
            for line in cnf_result[0].splitlines():
                if line.strip().upper().startswith('STACK'):
                    if '=' in line:
                        val = line.split('=', 1)[1].strip()
                        if val.startswith('0x') or val.startswith('0X'):
                            stack_from_cnf = val.lower()
        print(f"  stack_base    = \"{stack_from_cnf}\"  # from SYSTEM.CNF STACK=")
    print(f"  disc          = \"gex/gex.cue\"")
    print(f"  ──────────────────────────")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    bin_path = sys.argv[1]
    if not os.path.exists(bin_path):
        print(f"ERROR: file not found: {bin_path}")
        sys.exit(1)

    if len(sys.argv) >= 3:
        output_path = sys.argv[2]
    else:
        base = os.path.splitext(os.path.basename(bin_path))[0]
        output_path = f"{base}.exe"

    extract_exe(bin_path, output_path)
