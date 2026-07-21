#!/usr/bin/env python3
"""
extract_overlays.py — Scan a PSX disc image for overlay byte regions.

Reads overlay_map.jsonl (committed metadata: crc32, load_addr, size).
Scans the disc via ISO 9660 file enumeration, computing CRC32 of each
file's bytes. When a file matches an overlay entry, writes its bytes to
overlays/<crc32>.bin for the recompiler (B-2).

Falls back to sector-by-sector scan for any overlays not found via ISO.

Supports:
  BIN/CUE  MODE2/2352 — standard PSX disc rip (2352 bytes/sector)
  ISO      2048 bytes/sector — less common but handled

Usage:
  python3 tools/extract_overlays.py \\
      --disc   <path/to/game.cue | game.bin | game.iso> \\
      --manifest <overlay_map.jsonl> \\
      --out    <output_dir>

Outputs:
  <out>/<CRC32HEX>.bin  for each matched overlay
"""

import sys
import os
import re
import struct
import argparse
import binascii

# ---------------------------------------------------------------------------
# Disc sector reader
# ---------------------------------------------------------------------------

SECTOR_RAW   = 2352   # MODE2/2352
SECTOR_DATA  = 2048   # user data per sector
DATA_OFFSET  = 24     # offset of user data in raw MODE2 sector

class DiscReader:
    """Reads user-data sectors from a PSX BIN/CUE or ISO image."""

    def __init__(self, bin_path, raw=True):
        self.f       = open(bin_path, 'rb')
        self.raw     = raw   # True = MODE2/2352, False = 2048-byte ISO
        self.sec_sz  = SECTOR_RAW if raw else SECTOR_DATA
        fsize = os.path.getsize(bin_path)
        self.total   = fsize // self.sec_sz

    def close(self):
        self.f.close()

    def read_sector_data(self, lba):
        """Return 2048 user-data bytes for the given LBA."""
        offset = lba * self.sec_sz
        self.f.seek(offset)
        raw = self.f.read(self.sec_sz)
        if len(raw) < self.sec_sz:
            return b'\x00' * SECTOR_DATA
        if self.raw:
            return raw[DATA_OFFSET: DATA_OFFSET + SECTOR_DATA]
        return raw[:SECTOR_DATA]

    def read_file_bytes(self, lba, size):
        """Read exactly `size` bytes starting at `lba`."""
        sectors_needed = (size + SECTOR_DATA - 1) // SECTOR_DATA
        # Do not repeatedly append to an immutable bytes object: large archive
        # files (MMX6's ROCK_X6.DAT is ~51 MB / ~25k sectors) turn that into an
        # O(n^2) copy loop.  Accumulate references and copy the payload once.
        chunks = [self.read_sector_data(lba + i)
                  for i in range(sectors_needed)]
        return b''.join(chunks)[:size]

# ---------------------------------------------------------------------------
# CUE parser — extracts BIN path and track mode
# ---------------------------------------------------------------------------

def parse_cue(cue_path):
    """Return (bin_path, is_raw) from a .cue file."""
    cue_dir = os.path.dirname(os.path.abspath(cue_path))
    bin_path = None
    is_raw   = True   # default: MODE2/2352
    with open(cue_path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'\s*FILE\s+"([^"]+)"\s+BINARY', line, re.IGNORECASE)
            if m:
                bin_path = os.path.join(cue_dir, m.group(1))
            m = re.match(r'\s*TRACK\s+\d+\s+(MODE\d/(\d+))', line, re.IGNORECASE)
            if m:
                is_raw = (int(m.group(2)) == SECTOR_RAW)
    if bin_path is None:
        raise ValueError(f'No BINARY FILE found in {cue_path}')
    return bin_path, is_raw

# ---------------------------------------------------------------------------
# ISO 9660 directory walker
# ---------------------------------------------------------------------------

def _read_pvd(disc, lba=16):
    """Return root dir LBA and size from the Primary Volume Descriptor."""
    data = disc.read_sector_data(lba)
    if data[0] != 1 or data[1:6] != b'CD001':
        raise ValueError('Sector 16 is not an ISO 9660 PVD')
    root = data[156:190]
    root_lba  = struct.unpack_from('<I', root, 2)[0]
    root_size = struct.unpack_from('<I', root, 10)[0]
    return root_lba, root_size

def _iter_dir(disc, lba, size):
    """Yield (name, file_lba, file_size, is_dir) for each entry in a dir."""
    raw = disc.read_file_bytes(lba, size)
    pos = 0
    while pos < len(raw):
        rec_len = raw[pos]
        if rec_len == 0:
            # Skip to next sector boundary
            pos = ((pos // SECTOR_DATA) + 1) * SECTOR_DATA
            continue
        if pos + rec_len > len(raw):
            break
        rec      = raw[pos: pos + rec_len]
        flags    = rec[25]
        is_dir   = bool(flags & 0x02)
        elba     = struct.unpack_from('<I', rec, 2)[0]
        esize    = struct.unpack_from('<I', rec, 10)[0]
        name_len = rec[32]
        name     = rec[33: 33 + name_len].decode('ascii', errors='replace')
        name     = name.split(';')[0]   # strip version suffix
        if name not in ('', '\x00', '\x01'):
            yield name, elba, esize, is_dir
        pos += rec_len

def enumerate_files(disc, dir_lba=None, dir_size=None, prefix=''):
    """Recursively yield (path, lba, size) for every file on the disc."""
    if dir_lba is None:
        dir_lba, dir_size = _read_pvd(disc)
    for name, lba, size, is_dir in _iter_dir(disc, dir_lba, dir_size):
        path = f'{prefix}/{name}' if prefix else name
        if is_dir:
            yield from enumerate_files(disc, lba, size, path)
        else:
            yield path, lba, size

# ---------------------------------------------------------------------------
# Overlay manifest
# ---------------------------------------------------------------------------

def load_manifest(jsonl_path):
    import json
    entries = []
    with open(jsonl_path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            e = json.loads(line)
            lba_raw = e.get('start_lba', 'unknown')
            entries.append({
                'crc32':      int(e['crc32'], 16),
                'load_addr':  int(e['load_addr'], 16),
                'size':       int(e['size']),
                'start_lba':  int(lba_raw, 16) if lba_raw != 'unknown' else -1,
            })
    return entries

def crc32_of(data):
    return binascii.crc32(data) & 0xFFFFFFFF

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--disc',     required=True,
                    help='Path to .cue, .bin, or .iso disc image')
    ap.add_argument('--manifest', required=True,
                    help='overlay_map.jsonl with (crc32, load_addr, size)')
    ap.add_argument('--out',      default='overlays',
                    help='Output directory for .bin files (default: overlays/)')
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    # Open disc
    disc_path = args.disc
    if disc_path.lower().endswith('.cue'):
        bin_path, is_raw = parse_cue(disc_path)
    else:
        bin_path = disc_path
        is_raw   = not disc_path.lower().endswith('.iso')

    print(f'Disc : {bin_path} ({"MODE2/2352" if is_raw else "ISO 2048"})')
    disc = DiscReader(bin_path, raw=is_raw)

    overlays = load_manifest(args.manifest)
    print(f'Manifest: {len(overlays)} overlay(s) to find')
    print()

    # Build lookup by crc32
    remaining = {o['crc32']: o for o in overlays}
    found     = {}

    # --- Pass 0: Direct LBA extraction (fastest — uses cd_read_log data) ---
    print('Pass 0: direct LBA extraction (from cd_read_log)...')
    for crc, o in list(remaining.items()):
        lba = o.get('start_lba', -1)
        if lba < 0:
            continue
        size = o['size']
        data = disc.read_file_bytes(lba, size)
        actual_crc = crc32_of(data)
        if actual_crc == crc:
            remaining.pop(crc)
            out_name = f'{crc:08X}.bin'
            out_path = os.path.join(args.out, out_name)
            with open(out_path, 'wb') as f:
                f.write(data)
            found[crc] = (f'lba:{lba}(direct)', lba, size)
            print(f'  FOUND  lba={lba}  load=0x{o["load_addr"]:08X}'
                  f'  size={size}  -> {out_name}')
        else:
            print(f'  MISMATCH  lba={lba}  expected=0x{crc:08X}'
                  f'  got=0x{actual_crc:08X}  (will try ISO/brute-force)')

    # --- Pass 1: ISO 9660 file scan (fast) ---
    print('Pass 1: ISO 9660 file enumeration...')
    try:
        for path, lba, size in enumerate_files(disc):
            if not remaining:
                break
            # Try exact file size and also overlay-truncated size
            for try_size in sorted({size, *[o['size'] for o in remaining.values()]}):
                if try_size > size + SECTOR_DATA:
                    continue   # file too small to cover this overlay
                data = disc.read_file_bytes(lba, try_size)
                crc  = crc32_of(data[:try_size])
                if crc in remaining:
                    o = remaining.pop(crc)
                    out_name = f'{crc:08X}.bin'
                    out_path = os.path.join(args.out, out_name)
                    with open(out_path, 'wb') as f:
                        f.write(data[:o['size']])
                    found[crc] = (path, lba, size)
                    print(f'  FOUND  {path:40s}  load=0x{o["load_addr"]:08X}'
                          f'  size={o["size"]}  -> {out_name}')
                    break
    except Exception as e:
        print(f'  ISO scan error: {e}')

    # --- Pass 2: brute-force sector scan for anything not found ---
    if remaining:
        print()
        print(f'Pass 2: brute-force sector scan for {len(remaining)} unfound overlay(s)...')
        for ov_crc, ov in list(remaining.items()):
            sz     = ov['size']
            nsec   = (sz + SECTOR_DATA - 1) // SECTOR_DATA
            print(f'  Scanning for crc32=0x{ov_crc:08X} size={sz} ({nsec} sectors)...',
                  flush=True)
            found_it = False
            for lba in range(disc.total - nsec + 1):
                data = disc.read_file_bytes(lba, sz)
                if crc32_of(data) == ov_crc:
                    remaining.pop(ov_crc)
                    out_name = f'{ov_crc:08X}.bin'
                    out_path = os.path.join(args.out, out_name)
                    with open(out_path, 'wb') as f:
                        f.write(data)
                    found[ov_crc] = (f'lba:{lba}', lba, sz)
                    print(f'  FOUND  lba={lba}  load=0x{ov["load_addr"]:08X}'
                          f'  size={sz}  -> {out_name}')
                    found_it = True
                    break
            if not found_it:
                print(f'  NOT FOUND')

    disc.close()

    print()
    print(f'Results: {len(found)} found  {len(remaining)} not found')
    if remaining:
        print('Not found (play through more content and re-run dump_overlays.py):')
        for crc, o in remaining.items():
            print(f'  0x{crc:08X}  load=0x{o["load_addr"]:08X}  size={o["size"]}')
    else:
        print('All overlays extracted successfully.')

if __name__ == '__main__':
    main()
