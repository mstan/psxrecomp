#!/usr/bin/env python3
"""gpu_frame_capture.py -- capture one frame's GP0 stream + screenshot, decoded
and attributed to the guest functions that issued it.

    # the newest frame the ring holds, saved as the "bad" reference
    python3 gpu_frame_capture.py --tag bad --out analysis/frames

    # a specific past frame, and the four before it
    python3 gpu_frame_capture.py --frame 41230 --frames 5 --tag bad

    # what can I still ask for?
    python3 gpu_frame_capture.py --ring

You do not pause the game to use this, and you cannot: pause / step /
run_to_frame were removed from psx-runtime (runtime/src/debug_server.c) because
pause-step-read synthesizes a snapshot instead of reading the history the
runtime already keeps. The GP0 ring holds ~1M packets -- several hundred frames
-- so the workflow is the other way round: play until the bug is on screen, then
capture that frame AND the ones leading up to it. Reaching backwards beats
freezing forwards, and it is the only thing that works for a glitch you cannot
stop on.

Writes <out>/<tag>.json (a psx-gpu-frame dump), <out>/<tag>.summary.json (the
compact attribution RetComM Studio reads), <out>/<tag>.png (the presented
frame), and <out>/<tag>.opcodes.json, so a later diff or layer render needs
nothing still running.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_NATIVE_PORT, DebugConn, DebugError, capture, save_dump, save_summary,
)


def summarise(dump, top=25, out=sys.stdout):
    """Per-function attribution table -- the thing you actually read."""
    funcs = dump["funcs"]
    order = sorted(funcs.values(), key=lambda r: (-r["drawing"], r["func"]))
    total_draw = sum(r["drawing"] for r in funcs.values())
    print(f"frame {dump['frame']}  packets={dump['raw_count']}  "
          f"drawing={total_draw}  functions={len(funcs)}", file=out)
    if dump.get("capped"):
        print(f"  !! ring returned the full {dump['requested']} requested -- the frame "
              f"may be truncated; re-run with a larger --count", file=out)
    trunc = sum(1 for p in dump["prims"] if p.get("truncated"))
    if trunc:
        print(f"  note: {trunc} packet(s) longer than the ring's word cap; "
              f"decoded as far as recorded", file=out)
    print(file=out)
    print(f"  {'func':<12} {'draw':>6} {'semi':>6} {'tex':>6} {'OT':>11}  ops", file=out)
    for r in order[:top]:
        ot = "-" if r["ot_min"] is None else (
            f"{r['ot_min']}" if r["ot_min"] == r["ot_max"] else f"{r['ot_min']}..{r['ot_max']}")
        ops = ", ".join(f"{k}x{v}" for k, v in
                        sorted(r["ops"].items(), key=lambda kv: -kv[1])[:4])
        print(f"  {r['func']:<12} {r['drawing']:>6} {r['semi']:>6} "
              f"{r['textured']:>6} {ot:>11}  {ops}", file=out)
    if len(order) > top:
        print(f"  ... {len(order) - top} more function(s)", file=out)

    modes = {}
    for r in funcs.values():
        for m, n in r["stp_modes"].items():
            modes[m] = modes.get(m, 0) + n
    print(file=out)
    if modes:
        print("  semi-transparency modes in use: " +
              ", ".join(f"{m} x{n}" for m, n in sorted(modes.items())), file=out)
    else:
        print("  no semi-transparent primitives in this frame -- if the effect "
              "you are chasing is a glow or a vignette, that is the finding",
              file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--frame", type=int, default=None,
                    help="frame to dump (default: the newest one the ring holds)")
    ap.add_argument("--frames", type=int, default=1,
                    help="also capture the N-1 frames BEFORE --frame, walking "
                         "backwards through the ring (tag gets a -NN suffix, "
                         "-00 being the newest)")
    ap.add_argument("--ring", action="store_true",
                    help="print which frames the GP0 ring can still be asked "
                         "for, then exit")
    ap.add_argument("--count", type=int, default=65536, help="max GP0 packets to pull")
    ap.add_argument("--tag", default="frame", help="output basename")
    ap.add_argument("--out", default="analysis/frames", help="output directory")
    ap.add_argument("--no-save", action="store_true", help="print only, write nothing")
    ap.add_argument("--no-shot", action="store_true", help="skip the screenshot")
    ap.add_argument("--summary", action="store_true", help="print the attribution table")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args(argv)

    try:
        with DebugConn(args.host, args.port, args.timeout) as conn:
            span = conn.ring_span()
            if args.ring:
                print(f"GP0 ring: {span['total']} packet(s) seen, capacity "
                      f"{span['capacity']}, {span['max_words']} words/packet")
                if span["total"] == 0:
                    print("  nothing recorded yet -- is a game running?")
                else:
                    print(f"  capturable frames: {span['oldest']}..{span['newest']}")
                return 0

            outdir = os.path.abspath(args.out)
            if not args.no_save:
                os.makedirs(outdir, exist_ok=True)

            base_frame = args.frame if args.frame is not None else span["newest"]
            n = max(1, args.frames)
            print(f"ring holds frames {span['oldest']}..{span['newest']}; "
                  f"capturing {n} frame(s) ending at {base_frame}")
            for i in range(n):
                # Walk BACKWARDS: the ring is history, and the frames before the
                # one you noticed are the ones that explain it.
                frame = base_frame - i
                if frame < span["oldest"]:
                    print(f"  stopping at {frame}: older than the ring holds "
                          f"({span['oldest']})", file=sys.stderr)
                    break
                tag = args.tag if n == 1 else f"{args.tag}-{i:02d}"
                dump = capture(conn, frame=frame, count=args.count, label=tag,
                               verify_ring=False)

                if not args.no_save:
                    jp = os.path.join(outdir, f"{tag}.json")
                    save_dump(dump, jp)
                    save_summary(dump, os.path.join(outdir, f"{tag}.summary.json"),
                                 dump_name=f"{tag}.json")
                    print(f"wrote {jp}  ({dump['raw_count']} packets, "
                          f"{len(dump['funcs'])} functions)")
                    try:
                        oc = conn.cmd("gpu_opcodes")
                        with open(os.path.join(outdir, f"{tag}.opcodes.json"), "w",
                                  encoding="utf-8") as f:
                            json.dump(oc.get("opcodes", {}), f, indent=1)
                    except DebugError as e:
                        print(f"  (gpu_opcodes unavailable: {e})", file=sys.stderr)
                    if not args.no_shot:
                        if frame != span["newest"]:
                            print("  (no screenshot: the framebuffer holds the "
                                  "live frame, not this historical one)")
                        else:
                            try:
                                r = conn.screenshot(os.path.join(outdir, f"{tag}.png"))
                                print(f"wrote {r.get('path')}")
                            except DebugError as e:
                                print(f"  (screenshot failed: {e})", file=sys.stderr)

                if args.summary or args.no_save:
                    summarise(dump, top=args.top)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
