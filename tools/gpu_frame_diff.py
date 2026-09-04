#!/usr/bin/env python3
"""gpu_frame_diff.py -- diff two captured frames by the guest function that drew them.

    python3 gpu_frame_diff.py analysis/frames/good.json analysis/frames/bad.json

The report leads with the findings that actually name a bug:

  * a function that drew in A and drew nothing in B  -> it stopped running, or
    stopped reaching its emit path
  * a function whose semi-transparent primitive count collapsed -> the STP bit
    or the draw-mode latch feeding it changed
  * a semi-transparency MODE that disappeared entirely -> whatever layer used
    that blend (a B-F vignette, a B+F glow) is no longer being drawn

Frame-to-frame jitter is real: an animating effect legitimately moves vertices
every frame. So the diff is over counts and flags per (function, opcode), never
over exact geometry, and geometry is only ever quoted as a sample for context.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import STP_MODES, load_dump  # noqa: E402


def bucket(dump):
    """Counts keyed (func, op_name) plus per-function and global rollups."""
    per_key = {}
    per_func = {}
    ops = {}
    modes = {}
    for p in dump["prims"]:
        if p["kind"] not in ("poly", "rect", "line", "fill"):
            continue
        fn = p.get("func", "0x00000000")
        key = (fn, p["op_name"])
        b = per_key.setdefault(key, {"n": 0, "semi": 0, "tex": 0, "sample": None})
        b["n"] += 1
        if p.get("semi"):
            b["semi"] += 1
        if p.get("textured"):
            b["tex"] += 1
        if b["sample"] is None:
            b["sample"] = {"verts": p.get("verts"), "colors": p.get("colors"),
                           "stp": p.get("stp"), "ot": p.get("ot"),
                           "ra": p.get("ra"), "seq": p.get("seq")}
        f = per_func.setdefault(fn, {"n": 0, "semi": 0, "tex": 0, "ras": set()})
        f["n"] += 1
        if p.get("semi"):
            f["semi"] += 1
        if p.get("textured"):
            f["tex"] += 1
        if p.get("ra"):
            f["ras"].add(p["ra"])
        ops[p["op_name"]] = ops.get(p["op_name"], 0) + 1
        if p.get("semi"):
            m = STP_MODES.get(p.get("stp", 0), "?")
            modes[m] = modes.get(m, 0) + 1
    for f in per_func.values():
        f["ras"] = sorted(f["ras"])
    return {"keys": per_key, "funcs": per_func, "ops": ops, "modes": modes}


def _delta_map(a, b):
    out = {}
    for k in set(a) | set(b):
        av, bv = a.get(k, 0), b.get(k, 0)
        if av != bv:
            out[k] = {"a": av, "b": bv, "delta": bv - av}
    return out


def diff(dump_a, dump_b):
    A, B = bucket(dump_a), bucket(dump_b)
    funcs = []
    for fn in sorted(set(A["funcs"]) | set(B["funcs"])):
        fa = A["funcs"].get(fn, {"n": 0, "semi": 0, "tex": 0, "ras": []})
        fb = B["funcs"].get(fn, {"n": 0, "semi": 0, "tex": 0, "ras": []})
        if fa["n"] == fb["n"] and fa["semi"] == fb["semi"] and fa["tex"] == fb["tex"]:
            continue
        if fa["n"] and not fb["n"]:
            verdict = "stopped drawing"
        elif fb["n"] and not fa["n"]:
            verdict = "started drawing"
        elif fa["semi"] and not fb["semi"]:
            verdict = "lost semi-transparency"
        elif fb["semi"] and not fa["semi"]:
            verdict = "gained semi-transparency"
        else:
            verdict = "count changed"
        funcs.append({
            "func": fn, "verdict": verdict,
            "a": {"prims": fa["n"], "semi": fa["semi"], "tex": fa["tex"]},
            "b": {"prims": fb["n"], "semi": fb["semi"], "tex": fb["tex"]},
            "ras": sorted(set(fa["ras"]) | set(fb["ras"]))[:8],
        })

    keys = []
    for k in sorted(set(A["keys"]) | set(B["keys"])):
        ka = A["keys"].get(k, {"n": 0, "semi": 0, "tex": 0, "sample": None})
        kb = B["keys"].get(k, {"n": 0, "semi": 0, "tex": 0, "sample": None})
        if ka["n"] == kb["n"] and ka["semi"] == kb["semi"]:
            continue
        keys.append({
            "func": k[0], "op": k[1],
            "a": {"n": ka["n"], "semi": ka["semi"], "sample": ka["sample"]},
            "b": {"n": kb["n"], "semi": kb["semi"], "sample": kb["sample"]},
        })

    order = {"stopped drawing": 0, "lost semi-transparency": 1, "started drawing": 2,
             "gained semi-transparency": 3, "count changed": 4}
    funcs.sort(key=lambda f: (order.get(f["verdict"], 9),
                              -abs(f["b"]["prims"] - f["a"]["prims"])))
    return {
        "kind": "psx-gpu-frame-diff",
        "version": 1,
        "a": {"label": dump_a.get("label", ""), "frame": dump_a["frame"],
              "packets": dump_a["raw_count"]},
        "b": {"label": dump_b.get("label", ""), "frame": dump_b["frame"],
              "packets": dump_b["raw_count"]},
        "funcs": funcs,
        "keys": keys,
        "ops": _delta_map(A["ops"], B["ops"]),
        "modes": _delta_map(A["modes"], B["modes"]),
        "modes_a": A["modes"],
        "modes_b": B["modes"],
    }


def report(d, out=sys.stdout, max_keys=40):
    print(f"A {d['a']['label'] or 'a'}: frame {d['a']['frame']}, "
          f"{d['a']['packets']} packets", file=out)
    print(f"B {d['b']['label'] or 'b'}: frame {d['b']['frame']}, "
          f"{d['b']['packets']} packets", file=out)
    print(file=out)

    gone = [m for m, v in d["modes"].items() if v["b"] == 0 and v["a"] > 0]
    new = [m for m, v in d["modes"].items() if v["a"] == 0 and v["b"] > 0]
    if gone:
        print("HEADLINE: semi-transparency mode(s) present in A and absent in B: "
              + ", ".join(f"{m} (was {d['modes'][m]['a']} prims)" for m in gone), file=out)
    if new:
        print("HEADLINE: semi-transparency mode(s) new in B: "
              + ", ".join(f"{m} ({d['modes'][m]['b']} prims)" for m in new), file=out)
    stopped = [f for f in d["funcs"] if f["verdict"] == "stopped drawing"]
    if stopped:
        print(f"HEADLINE: {len(stopped)} function(s) drew in A and nothing in B: "
              + ", ".join(f["func"] for f in stopped[:6])
              + (" ..." if len(stopped) > 6 else ""), file=out)
    if not (gone or new or stopped):
        print("No function stopped drawing and no blend mode disappeared.", file=out)
    print(file=out)

    print("semi-transparency modes    A       B", file=out)
    for m in sorted(set(d["modes_a"]) | set(d["modes_b"])):
        print(f"  {m:<22} {d['modes_a'].get(m, 0):>5}   {d['modes_b'].get(m, 0):>5}",
              file=out)
    print(file=out)

    if d["ops"]:
        print("opcode count changes", file=out)
        for op, v in sorted(d["ops"].items(), key=lambda kv: -abs(kv[1]["delta"]))[:20]:
            print(f"  {op:<22} {v['a']:>5} -> {v['b']:>5}  ({v['delta']:+d})", file=out)
        print(file=out)

    print(f"per-function changes ({len(d['funcs'])})", file=out)
    print(f"  {'func':<12} {'verdict':<24} {'A prims/semi':>14} {'B prims/semi':>14}",
          file=out)
    for f in d["funcs"][:max_keys]:
        a = f"{f['a']['prims']}/{f['a']['semi']}"
        b = f"{f['b']['prims']}/{f['b']['semi']}"
        print(f"  {f['func']:<12} {f['verdict']:<24} {a:>14} {b:>14}", file=out)
    if len(d["funcs"]) > max_keys:
        print(f"  ... {len(d['funcs']) - max_keys} more", file=out)
    print(file=out)

    print(f"per (function, opcode) changes ({len(d['keys'])})", file=out)
    for k in d["keys"][:max_keys]:
        print(f"  {k['func']:<12} {k['op']:<20} "
              f"{k['a']['n']:>5} -> {k['b']['n']:>5}   "
              f"semi {k['a']['semi']:>4} -> {k['b']['semi']:>4}", file=out)
    if len(d["keys"]) > max_keys:
        print(f"  ... {len(d['keys']) - max_keys} more", file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="reference (good) psx-gpu-frame dump")
    ap.add_argument("b", help="suspect (bad) psx-gpu-frame dump")
    ap.add_argument("--json", default=None, help="also write the diff as JSON here")
    ap.add_argument("--max", type=int, default=40)
    ap.add_argument("--quiet", action="store_true", help="write JSON only")
    args = ap.parse_args(argv)

    d = diff(load_dump(args.a), load_dump(args.b))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=1)
    if not args.quiet:
        report(d, max_keys=args.max)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
