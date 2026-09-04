#!/usr/bin/env python3
"""gpu_frame_layers.py -- render a captured frame as one image per guest function.

    python3 gpu_frame_layers.py analysis/frames/bad.json --out analysis/frames/bad-layers

You get:
  composite.png       every primitive, in issue order
  layer-<func>.png    that function's contribution alone (RGBA, alpha=coverage)
  sheet.png           a labelled contact sheet of all layers
  layers.json         the index: per-function stats, bbox, file names

What this is and is not
-----------------------
It rasterises geometry and shading, and it applies the four real semi-
transparency blends, because "the glow is opaque" and "the vignette never got
drawn" are both blend-state findings and both invisible in a wireframe.

It does NOT sample textures. A textured primitive is filled with its command
colour (or flat grey when the raw-texture bit says the command colour is
ignored). Texture pages, CLUTs and UVs are decoded and reported in layers.json,
so a wrong-CLUT hypothesis is testable, but this is not a second GPU.

That trade is deliberate: the question these layers answer is *which function
drew what, and with which blend*, and re-implementing texture sampling would
buy fidelity the question does not need while adding a whole new way to be
wrong.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402

from psx_gpu_frame import STP_MODES, draw_area, load_dump, prim_bbox  # noqa: E402

RAW_TEXTURE_GREY = (128.0, 128.0, 128.0)
TEX_TINT = (255.0, 0.0, 255.0)

# Isolated layers render over a neutral grey, not black. A B-F subtractive
# vignette against black is black, and an additive glow against black is just
# the glow -- both blends become invisible in exactly the view meant to show
# them. The composite still starts from black, which is what the GPU sees.
LAYER_BACKDROP = 128.0


class Canvas:
    """Float RGB canvas in VRAM coordinates, offset to the frame's draw area."""

    def __init__(self, x0: int, y0: int, w: int, h: int, backdrop: float = 0.0):
        self.x0, self.y0 = x0, y0
        self.w, self.h = w, h
        self.backdrop = float(backdrop)
        self.rgb = np.full((h, w, 3), float(backdrop), dtype=np.float32)
        self.cov = np.zeros((h, w), dtype=bool)

    def _blend(self, mask, src, semi, stp):
        """Apply one primitive's pixels. src is (...,3) float or a 3-vector.

        The four modes are GPUSTAT bits 5-6 exactly: 0.5B+0.5F, B+F, B-F,
        B+0.25F. Getting these right is the point of rendering at all.
        """
        dst = self.rgb[mask]
        if not semi:
            out = src
        elif stp == 0:
            out = 0.5 * dst + 0.5 * src
        elif stp == 1:
            out = dst + src
        elif stp == 2:
            out = dst - src
        else:
            out = dst + 0.25 * src
        self.rgb[mask] = np.clip(out, 0.0, 255.0)
        self.cov[mask] = True

    def triangle(self, pts, cols, semi, stp):
        (ax, ay), (bx, by), (cx, cy) = pts
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if den == 0:
            return
        minx = max(0, int(np.floor(min(ax, bx, cx))) - self.x0)
        maxx = min(self.w - 1, int(np.ceil(max(ax, bx, cx))) - self.x0)
        miny = max(0, int(np.floor(min(ay, by, cy))) - self.y0)
        maxy = min(self.h - 1, int(np.ceil(max(ay, by, cy))) - self.y0)
        if minx > maxx or miny > maxy:
            return
        # Oversize primitives are rejected by the hardware too (see
        # psx_gpu_triangle_oversize in gpu.c); a 1000px triangle here is almost
        # always a decode or a data bug, and filling it would hide the real ones.
        if (maxx - minx) > 1024 or (maxy - miny) > 512:
            return
        xs = np.arange(minx, maxx + 1, dtype=np.float32) + self.x0
        ys = np.arange(miny, maxy + 1, dtype=np.float32) + self.y0
        X, Y = np.meshgrid(xs, ys)
        l0 = ((by - cy) * (X - cx) + (cx - bx) * (Y - cy)) / den
        l1 = ((cy - ay) * (X - cx) + (ax - cx) * (Y - cy)) / den
        l2 = 1.0 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        if not inside.any():
            return
        c = np.asarray(cols, dtype=np.float32)
        src = (l0[..., None] * c[0] + l1[..., None] * c[1] + l2[..., None] * c[2])[inside]
        full = np.zeros((self.h, self.w), dtype=bool)
        full[miny:maxy + 1, minx:maxx + 1] = inside
        self._blend(full, src, semi, stp)

    def rect(self, x, y, w, h, color, semi, stp):
        x0 = max(0, x - self.x0)
        y0 = max(0, y - self.y0)
        x1 = min(self.w, x - self.x0 + max(0, w))
        y1 = min(self.h, y - self.y0 + max(0, h))
        if x1 <= x0 or y1 <= y0:
            return
        mask = np.zeros((self.h, self.w), dtype=bool)
        mask[y0:y1, x0:x1] = True
        self._blend(mask, np.asarray(color, dtype=np.float32), semi, stp)

    def line(self, p0, p1, c0, c1, semi, stp):
        (x0, y0), (x1, y1) = p0, p1
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        if n > 4096:
            return
        t = np.linspace(0.0, 1.0, n)
        xs = np.rint(x0 + (x1 - x0) * t).astype(int) - self.x0
        ys = np.rint(y0 + (y1 - y0) * t).astype(int) - self.y0
        ok = (xs >= 0) & (xs < self.w) & (ys >= 0) & (ys < self.h)
        if not ok.any():
            return
        cols = (np.asarray(c0, dtype=np.float32) * (1 - t)[:, None] +
                np.asarray(c1, dtype=np.float32) * t[:, None])
        mask = np.zeros((self.h, self.w), dtype=bool)
        mask[ys[ok], xs[ok]] = True
        # Duplicated pixels along a steep line would blend twice; take the
        # first colour per pixel instead, which is what a scanline does.
        acc = np.zeros((self.h, self.w, 3), dtype=np.float32)
        acc[ys[ok], xs[ok]] = cols[ok]
        self._blend(mask, acc[mask], semi, stp)

    def to_rgb_image(self):
        return Image.fromarray(self.rgb.astype(np.uint8), "RGB")

    def to_rgba_image(self):
        rgba = np.zeros((self.h, self.w, 4), dtype=np.uint8)
        rgba[..., :3] = self.rgb.astype(np.uint8)
        rgba[..., 3] = np.where(self.cov, 255, 0)
        return Image.fromarray(rgba, "RGBA")


def prim_color(p, tex_tint: bool):
    if p.get("textured"):
        if tex_tint:
            return TEX_TINT
        if p.get("raw_tex"):
            return RAW_TEXTURE_GREY
    return None


def draw_prim(canvas: Canvas, p, tex_tint: bool):
    """Rasterise one decoded primitive. Returns True if it put down pixels."""
    kind = p["kind"]
    if kind not in ("poly", "rect", "line", "fill"):
        return False
    verts = p.get("verts") or []
    cols = p.get("colors") or []
    if not verts:
        return False
    override = prim_color(p, tex_tint)
    if override is not None:
        cols = [list(override)] * len(verts)
    while len(cols) < len(verts):
        cols.append(cols[-1] if cols else [128, 128, 128])
    semi = bool(p.get("semi"))
    stp = int(p.get("stp", 0))

    if kind == "poly":
        if len(verts) >= 3:
            canvas.triangle(verts[0:3], cols[0:3], semi, stp)
        if len(verts) >= 4:
            canvas.triangle([verts[1], verts[2], verts[3]],
                            [cols[1], cols[2], cols[3]], semi, stp)
        return len(verts) >= 3
    if kind in ("rect", "fill"):
        size = p.get("size") or [1, 1]
        # GP0(02) fill is opaque and ignores blending entirely.
        f_semi = semi and kind != "fill"
        canvas.rect(verts[0][0], verts[0][1], size[0], size[1], cols[0], f_semi, stp)
        return True
    if kind == "line":
        drew = False
        for i in range(len(verts) - 1):
            canvas.line(verts[i], verts[i + 1], cols[i], cols[i + 1], semi, stp)
            drew = True
        return drew
    return False


def safe_name(func: str) -> str:
    return re.sub(r"[^0-9A-Za-z_.-]", "_", func)


def render(dump, out_dir, scale=1, tex_tint=False, order="issue",
           max_layers=64, only=None, exclude=None, sheet=True,
           layer_backdrop=LAYER_BACKDROP):
    x0, y0, x1, y1 = draw_area(dump)
    w, h = min(1024, x1 - x0), min(512, y1 - y0)
    if w <= 0 or h <= 0:
        raise SystemExit("frame has no drawable area")

    prims = list(dump["prims"])
    if order == "ot":
        prims.sort(key=lambda p: (p.get("ot", 0xFFFF), p.get("seq", 0)))
    if only:
        prims = [p for p in prims if p.get("func") in only]
    if exclude:
        prims = [p for p in prims if p.get("func") not in exclude]

    os.makedirs(out_dir, exist_ok=True)

    composite = Canvas(x0, y0, w, h)
    per_func = {}
    for p in prims:
        drew = draw_prim(composite, p, tex_tint)
        if drew:
            per_func.setdefault(p.get("func", "0x00000000"), []).append(p)

    def save(img, name):
        if scale > 1:
            img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
        path = os.path.join(out_dir, name)
        img.save(path)
        return path

    save(composite.to_rgb_image(), "composite.png")

    src_funcs = dump.get("funcs", {})
    ranked = sorted(per_func.items(), key=lambda kv: -len(kv[1]))
    index = {
        "kind": "psx-gpu-layers",
        "version": 1,
        "frame": dump["frame"],
        "label": dump.get("label", ""),
        "area": [x0, y0, x1, y1],
        "scale": scale,
        "order": order,
        "tex_tint": tex_tint,
        "layer_backdrop": layer_backdrop,
        "composite": "composite.png",
        "sheet": "sheet.png" if sheet else None,
        "rendered": sum(len(v) for v in per_func.values()),
        "non_drawing": len(prims) - sum(len(v) for v in per_func.values()),
        "layers": [],
    }

    thumbs = []
    for func, plist in ranked[:max_layers]:
        c = Canvas(x0, y0, w, h, backdrop=layer_backdrop)
        for p in plist:
            draw_prim(c, p, tex_tint)
        name = f"layer-{safe_name(func)}.png"
        save(c.to_rgba_image(), name)
        stats = src_funcs.get(func, {})
        bbox = None
        for p in plist:
            b = prim_bbox(p)
            if b:
                bbox = b if bbox is None else [min(bbox[0], b[0]), min(bbox[1], b[1]),
                                               max(bbox[2], b[2]), max(bbox[3], b[3])]
        modes = stats.get("stp_modes", {})
        index["layers"].append({
            "func": func,
            "file": name,
            "prims": len(plist),
            "semi": sum(1 for p in plist if p.get("semi")),
            "textured": sum(1 for p in plist if p.get("textured")),
            "pixels": int(c.cov.sum()),
            "bbox": bbox,
            "ot_min": stats.get("ot_min"),
            "ot_max": stats.get("ot_max"),
            "ops": stats.get("ops", {}),
            "stp_modes": modes,
            "ras": stats.get("ras", []),
        })
        thumbs.append((func, c, len(plist), sum(1 for p in plist if p.get("semi"))))

    if len(ranked) > max_layers:
        index["truncated_layers"] = len(ranked) - max_layers

    if sheet and thumbs:
        save_sheet(thumbs, os.path.join(out_dir, "sheet.png"), w, h,
                   backdrop=layer_backdrop)

    with open(os.path.join(out_dir, "layers.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
    return index


def save_sheet(thumbs, path, w, h, cols=4, pad=8, label_h=16, backdrop=LAYER_BACKDROP):
    """Labelled contact sheet -- the whole frame's authorship on one page."""
    tw = max(96, min(240, w))
    th = max(1, int(round(h * tw / max(1, w))))
    rows = (len(thumbs) + cols - 1) // cols
    W = cols * (tw + pad) + pad
    H = rows * (th + label_h + pad) + pad
    sheet = Image.new("RGB", (W, H), (18, 20, 24))
    d = ImageDraw.Draw(sheet)
    for i, (func, c, n, semi) in enumerate(thumbs):
        r, col = divmod(i, cols)
        x = pad + col * (tw + pad)
        y = pad + r * (th + label_h + pad)
        bg = int(round(backdrop * 0.35))
        tile = Image.new("RGB", (c.w, c.h), (bg, bg, bg))
        rgba = c.to_rgba_image()
        tile.paste(rgba.convert("RGB"), (0, 0), rgba.split()[3])
        sheet.paste(tile.resize((tw, th), Image.NEAREST), (x, y))
        d.rectangle([x, y, x + tw - 1, y + th - 1], outline=(70, 78, 90))
        d.text((x + 2, y + th + 2), f"{func}  {n}p" + (f"  {semi} semi" if semi else ""),
               fill=(200, 210, 220))
    sheet.save(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="a psx-gpu-frame JSON written by gpu_frame_capture.py")
    ap.add_argument("--out", default=None, help="output directory (default: <dump>-layers)")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--order", choices=("issue", "ot"), default="issue",
                    help="issue order (what the GPU saw) or OT rank order")
    ap.add_argument("--tex-tint", action="store_true",
                    help="draw textured prims flat magenta so texture coverage stands out")
    ap.add_argument("--max-layers", type=int, default=64)
    ap.add_argument("--only", action="append", default=None, metavar="0x8004ABCD")
    ap.add_argument("--exclude", action="append", default=None, metavar="0x8004ABCD")
    ap.add_argument("--no-sheet", action="store_true")
    ap.add_argument("--layer-backdrop", type=float, default=LAYER_BACKDROP,
                    help="grey level layers are rendered over so additive and "
                         "subtractive blends stay visible (0 = black, as the "
                         "composite is drawn)")
    args = ap.parse_args(argv)

    dump = load_dump(args.dump)
    out = args.out or os.path.splitext(args.dump)[0] + "-layers"
    idx = render(dump, out, scale=args.scale, tex_tint=args.tex_tint, order=args.order,
                 max_layers=args.max_layers,
                 only=set(args.only) if args.only else None,
                 exclude=set(args.exclude) if args.exclude else None,
                 sheet=not args.no_sheet, layer_backdrop=args.layer_backdrop)
    print(f"frame {idx['frame']}  area={idx['area']}  "
          f"rendered={idx['rendered']} non-drawing={idx['non_drawing']}  "
          f"layer backdrop={idx['layer_backdrop']:g}")
    print(f"  {len(idx['layers'])} layer(s) -> {out}")
    for l in idx["layers"][:20]:
        modes = ", ".join(f"{k}x{v}" for k, v in l["stp_modes"].items()) or "-"
        print(f"  {l['func']:<12} {l['prims']:>5}p {l['semi']:>4}semi "
              f"{l['pixels']:>8}px  bbox={l['bbox']}  stp={modes}")
    if idx.get("truncated_layers"):
        print(f"  ... {idx['truncated_layers']} more function(s) not rendered "
              f"(raise --max-layers)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
