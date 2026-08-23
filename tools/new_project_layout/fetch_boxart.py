#!/usr/bin/env python3
"""Fetch PS1 Named_Boxarts from libretro-thumbnails (PNG + launcher TGA).

Sources (tried in order):
  https://thumbnails.libretro.com/Sony - PlayStation/Named_Boxarts/<name>.png
  https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master/Named_Boxarts/<name>.png

Writes the original PNG (README) and a 32-bit uncompressed TGA (bottom-up BGRA)
for recomp-ui LAUNCHER_BOXART. Also writes launcher_assets/img/BOXART_SOURCE.txt
with URL + attribution.

Requires network. Prefer Pillow when installed; otherwise decodes common
8-bit RGBA/RGB PNGs with the stdlib.
"""

from __future__ import annotations

import argparse
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

LIBRETRO_HOST = "https://thumbnails.libretro.com/Sony%20-%20PlayStation/Named_Boxarts/"
GITHUB_RAW = (
    "https://raw.githubusercontent.com/libretro-thumbnails/"
    "Sony_-_PlayStation/master/Named_Boxarts/"
)
INVALID = '&*/:`<>?\\|"'


def sanitize_libretro_name(name: str) -> str:
    out = name.strip()
    if out.lower().endswith(".png"):
        out = out[:-4]
    for ch in INVALID:
        out = out.replace(ch, "_")
    return out


def candidate_names(*hints: str) -> list[str]:
    """Build Redump / libretro-style name candidates."""
    seen: list[str] = []

    def add(s: str) -> None:
        s = s.strip()
        if not s:
            return
        # Drop path / extension
        s = Path(s).name
        if s.lower().endswith(".cue"):
            s = s[:-4]
        s = sanitize_libretro_name(s)
        if s and s not in seen:
            seen.append(s)

    for h in hints:
        if not h:
            continue
        add(h)
        # Without trailing region if already present; with common regions.
        base = h
        for suf in (
            " (USA)",
            " (Europe)",
            " (Japan)",
            " (World)",
            " (En,Fr,De,Es,It)",
        ):
            if sanitize_libretro_name(base).endswith(sanitize_libretro_name(suf).strip()):
                continue
        stem = Path(h).stem if h.endswith(".cue") else h
        stem = stem.strip()
        # Strip region then re-add USA (most common Redump / libretro pairing).
        import re

        stripped = re.sub(
            r"\s*\((USA|Europe|Japan|World|En,Fr,De,Es,It)\)\s*$",
            "",
            stem,
            flags=re.I,
        ).strip()
        add(stripped)
        add(f"{stripped} (USA)")
        add(f"{stripped} (Europe)")
        add(f"{stripped} (Japan)")
        add(stem)

    return seen


def url_for(base: str, name: str) -> str:
    filename = sanitize_libretro_name(name) + ".png"
    return base + urllib.parse.quote(filename, safe="()-_!.'")


def http_get(url: str, timeout: float = 60.0) -> bytes:
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "psxrecomp-fetch_boxart/1.0"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = resp.read()
        ctype = (resp.headers.get("Content-Type") or "").lower()
        if "png" not in ctype and not data.startswith(b"\x89PNG"):
            raise RuntimeError(f"not a PNG ({ctype}): {url}")
        return data


def try_fetch(names: list[str]) -> tuple[bytes, str, str]:
    last_err: Exception | None = None
    for name in names:
        for base in (LIBRETRO_HOST, GITHUB_RAW):
            url = url_for(base, name)
            try:
                data = http_get(url)
                return data, url, name
            except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, RuntimeError) as e:
                last_err = e
                continue
    raise FileNotFoundError(
        f"boxart not found for candidates {names[:8]}… ({last_err})"
    )


def png_to_rgba(png: bytes) -> tuple[int, int, bytes]:
    """Return (w, h, RGBA bytes). Pillow preferred; stdlib RGBA/RGB PNG fallback."""
    try:
        from PIL import Image  # type: ignore
        import io

        im = Image.open(io.BytesIO(png)).convert("RGBA")
        return im.width, im.height, im.tobytes()
    except ImportError:
        pass

    return _decode_png_rgba_stdlib(png)


def _decode_png_rgba_stdlib(png: bytes) -> tuple[int, int, bytes]:
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("invalid PNG signature (install Pillow for more formats)")
    pos = 8
    width = height = 0
    bit_depth = 8
    color_type = -1
    idat = bytearray()
    while pos + 8 <= len(png):
        length = struct.unpack(">I", png[pos : pos + 4])[0]
        ctype = png[pos + 4 : pos + 8]
        data = png[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", data[:10])
        elif ctype == b"IDAT":
            idat.extend(data)
        elif ctype == b"IEND":
            break
    if width <= 0 or height <= 0:
        raise SystemExit("PNG missing IHDR")
    if bit_depth != 8 or color_type not in (2, 6):
        raise SystemExit(
            f"PNG color_type={color_type} bit_depth={bit_depth} unsupported; "
            "install Pillow (pip install pillow)"
        )
    raw = zlib.decompress(bytes(idat))
    bpp = 4 if color_type == 6 else 3
    stride = width * bpp
    rows: list[bytearray] = []
    i = 0
    prev = bytearray(stride)
    for _ in range(height):
        filt = raw[i]
        i += 1
        row = bytearray(raw[i : i + stride])
        i += stride
        if filt == 0:
            pass
        elif filt == 1:  # Sub
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + left) & 0xFF
        elif filt == 2:  # Up
            for x in range(stride):
                row[x] = (row[x] + prev[x]) & 0xFF
        elif filt == 3:  # Average
            for x in range(stride):
                left = row[x - bpp] if x >= bpp else 0
                row[x] = (row[x] + ((left + prev[x]) // 2)) & 0xFF
        elif filt == 4:  # Paeth
            for x in range(stride):
                a = row[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                row[x] = (row[x] + pr) & 0xFF
        else:
            raise SystemExit(f"unsupported PNG filter {filt}")
        rows.append(row)
        prev = row
    rgba = bytearray(width * height * 4)
    o = 0
    for row in rows:
        if bpp == 4:
            rgba[o : o + stride] = row
            o += stride
        else:
            for x in range(0, stride, 3):
                rgba[o : o + 3] = row[x : x + 3]
                rgba[o + 3] = 255
                o += 4
    return width, height, bytes(rgba)


def write_tga_bgra(path: Path, width: int, height: int, rgba: bytes) -> None:
    """Uncompressed 32-bit TGA, bottom-up, BGRA (recomp-ui friendly)."""
    if len(rgba) != width * height * 4:
        raise SystemExit("RGBA size mismatch")
    header = bytearray(18)
    header[2] = 2  # uncompressed true-color
    struct.pack_into("<H", header, 12, width)
    struct.pack_into("<H", header, 14, height)
    header[16] = 32
    header[17] = 0x28  # alpha 8 + origin top-left... use bottom-up: 0x08
    # 0x08 = 8-bit alpha attribute; origin bottom-left (0)
    header[17] = 0x08
    bgra = bytearray(width * height * 4)
    # Bottom-up: first stored row is image bottom
    for y in range(height):
        src_y = height - 1 - y
        for x in range(width):
            si = (src_y * width + x) * 4
            di = (y * width + x) * 4
            r, g, b, a = rgba[si : si + 4]
            bgra[di : di + 4] = bytes((b, g, r, a))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(header) + bytes(bgra))


def fetch_to_paths(
    tga_path: Path,
    *,
    cue_stem: str = "",
    display_name: str = "",
    extra_names: list[str] | None = None,
    png_path: Path | None = None,
) -> tuple[Path, Path, str]:
    """Fetch libretro Named_Boxarts PNG, write PNG + TGA + BOXART_SOURCE.txt."""
    names = candidate_names(cue_stem, display_name, *(extra_names or []))
    if not names:
        raise ValueError("pass cue_stem and/or display_name")
    png, url, matched = try_fetch(names)
    tga_path = Path(tga_path)
    png_path = Path(png_path) if png_path else tga_path.with_suffix(".png")
    png_path.parent.mkdir(parents=True, exist_ok=True)
    png_path.write_bytes(png)
    try:
        w, h, rgba = png_to_rgba(png)
        write_tga_bgra(tga_path, w, h, rgba)
    except SystemExit as exc:
        raise RuntimeError(str(exc) or "PNG decode failed") from exc
    src_path = tga_path.parent / "BOXART_SOURCE.txt"
    src_path.write_text(
        "Box art sourced from libretro-thumbnails (Named_Boxarts).\n"
        f"Matched name: {matched}\n"
        f"URL: {url}\n"
        "https://github.com/libretro-thumbnails/libretro-thumbnails\n"
        "PNG is for README; TGA is for recomp-ui LAUNCHER_BOXART.\n",
        encoding="utf-8",
    )
    return tga_path, png_path, url


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out",
        default="launcher_assets/img/boxart.tga",
        help="output TGA path (default: launcher_assets/img/boxart.tga)",
    )
    ap.add_argument("--cue-stem", default="", help="cue filename or stem hint")
    ap.add_argument("--display-name", default="", help="game display name hint")
    ap.add_argument("--name", action="append", default=[], help="extra libretro name candidate")
    ap.add_argument(
        "--source-out",
        default="",
        help="attribution file (default: next to TGA as BOXART_SOURCE.txt)",
    )
    ap.add_argument(
        "--png-out",
        default="",
        help="output PNG path (default: same stem as --out with .png)",
    )
    args = ap.parse_args()

    names = candidate_names(args.cue_stem, args.display_name, *args.name)
    if not names:
        print("error: pass --cue-stem and/or --display-name", file=sys.stderr)
        return 2

    print(f"  searching libretro boxart ({len(names)} candidates)…", file=sys.stderr)
    out = Path(args.out)
    png_out = Path(args.png_out) if args.png_out else out.with_suffix(".png")
    try:
        tga_path, png_path, url = fetch_to_paths(
            out,
            cue_stem=args.cue_stem,
            display_name=args.display_name,
            extra_names=list(args.name),
            png_path=png_out,
        )
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        print(str(exc) or "boxart fetch failed", file=sys.stderr)
        return 1
    print(f"  wrote {png_path}", file=sys.stderr)
    print(f"  wrote {tga_path}", file=sys.stderr)
    print(f"    {url}", file=sys.stderr)
    src_path = Path(args.source_out) if args.source_out else out.parent / "BOXART_SOURCE.txt"
    if args.source_out:
        src_path.write_text(
            (tga_path.parent / "BOXART_SOURCE.txt").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
    print(f"  wrote {src_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
