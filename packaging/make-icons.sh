#!/usr/bin/env bash
# Rasterize assets/psxrecomp.svg → PNG / ICO.
# Master PNG is 512x512 — linuxdeploy / AppImage reject oversized icons.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SVG="${ROOT}/assets/psxrecomp.svg"
PNG="${ROOT}/assets/psxrecomp.png"
ICO="${ROOT}/assets/psxrecomp.ico"
SIZE=512

if [[ ! -f "${SVG}" ]]; then
  echo "missing ${SVG}" >&2
  exit 1
fi

if command -v rsvg-convert >/dev/null 2>&1; then
  rsvg-convert -w "${SIZE}" -h "${SIZE}" "${SVG}" -o "${PNG}"
elif command -v magick >/dev/null 2>&1; then
  magick -background none "${SVG}" -resize "${SIZE}x${SIZE}" "${PNG}"
elif command -v convert >/dev/null 2>&1; then
  convert -background none "${SVG}" -resize "${SIZE}x${SIZE}" "${PNG}"
elif command -v inkscape >/dev/null 2>&1; then
  inkscape "${SVG}" -w "${SIZE}" -h "${SIZE}" -o "${PNG}"
else
  python3 - "${PNG}" "${SIZE}" <<'PY'
import struct, zlib, sys
from pathlib import Path

def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

out = Path(sys.argv[1])
w = h = int(sys.argv[2])
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        # Approximate dark rounded tile + teal pad blob (fallback only).
        cx, cy = x - w / 2, y - h / 2
        rr = (w * 0.42)
        inside = cx * cx + cy * cy < rr * rr
        if inside:
            t = (x + y) / (2 * w)
            r = int(26 + t * 10)
            g = int(35 + t * 10)
            b = int(50 + t * 10)
            a = 255
            # teal accent ring
            if abs((cx * cx + cy * cy) ** 0.5 - rr * 0.55) < w * 0.03:
                r, g, b, a = 45, 212, 191, 255
        else:
            r = g = b = a = 0
        row.extend([r, g, b, a])
    rows.append(bytes(row))
raw = b"".join(rows)
ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
out.write_bytes(png)
print(f"wrote {out} (python fallback)")
PY
fi

echo "PNG: ${PNG} (${SIZE}x${SIZE})"

if command -v magick >/dev/null 2>&1; then
  magick "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
elif command -v convert >/dev/null 2>&1; then
  convert "${PNG}" -define icon:auto-resize=256,128,64,48,32,16 "${ICO}"
  echo "ICO: ${ICO}"
else
  python3 - "${PNG}" "${ICO}" <<'PY'
# Minimal single-size ICO from PNG (no Pillow required).
import struct, sys, zlib
from pathlib import Path

def png_rgba(path):
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    i = 8
    w = h = None
    idat = b""
    while i < len(data):
        ln = struct.unpack(">I", data[i:i+4])[0]
        tag = data[i+4:i+8]
        chunk = data[i+8:i+8+ln]
        i += 12 + ln
        if tag == b"IHDR":
            w, h = struct.unpack(">II", chunk[:8])
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = 1 + w * 4
    pixels = bytearray()
    for y in range(h):
        row = raw[y*stride+1:(y+1)*stride]
        pixels.extend(row)
    return w, h, bytes(pixels)

w, h, rgba = png_rgba(sys.argv[1])
# ICO: BGRA + AND mask, bottom-up
stride = ((w * 32 + 31) // 32) * 4
and_stride = ((w + 31) // 32) * 4
xor = bytearray()
and_mask = bytearray()
for y in range(h-1, -1, -1):
    row = bytearray()
    for x in range(w):
        i = (y * w + x) * 4
        r, g, b, a = rgba[i:i+4]
        row += bytes([b, g, r, a])
    row += b"\x00" * (stride - w * 4)
    xor.extend(row)
for y in range(h-1, -1, -1):
    bits = 0
    byte = 0
    row = bytearray()
    for x in range(w):
        a = rgba[(y * w + x) * 4 + 3]
        byte = (byte << 1) | (0 if a > 0 else 1)
        bits += 1
        if bits == 8:
            row.append(byte)
            bits = byte = 0
    if bits:
        row.append(byte << (8 - bits))
    row += b"\x00" * (and_stride - len(row))
    and_mask.extend(row)

dib = struct.pack("<IIIHHIIIIII", 40, w, h*2, 1, 32, 0, len(xor), 0, 0, 0, 0) + xor + and_mask
# ICONDIR + ICONDIRENTRY
entry = struct.pack("<BBBBHHII", w if w < 256 else 0, h if h < 256 else 0, 0, 0, 1, 32, len(dib), 22)
ico = struct.pack("<HHH", 0, 1, 1) + entry + dib
Path(sys.argv[2]).write_bytes(ico)
print(f"wrote {sys.argv[2]} (python ico)")
PY
fi
