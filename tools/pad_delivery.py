#!/usr/bin/env python3
"""
pad_delivery.py — Decode the pad polls the game actually received over SIO.

pad_trace.py measures what the runtime *presented*; this measures what the
game *read*.  A single physical tap must appear as one contiguous run of
polls with the button held.  If the delivered stream drops back to "released"
in the middle of a press and then reports it held again, the game's edge
detector fires twice and the menu moves twice — from one tap.

Reads the always-on SIO byte ring (sio_trace) and reconstructs each pad
transaction:

    tx 0x01 -> rx 0xFF     address the controller
    tx 0x42 -> rx id       0x41 digital, 0x73 analog, 0xF3 config
    tx 0x00 -> rx 0x5A
    tx 0x00 -> rx btn_lo   active low
    tx 0x00 -> rx btn_hi
    tx 0x00 -> rx rx,ry,lx,ly   (analog only)

Usage:
    python3 pad_delivery.py --seconds 8 --json /tmp/delivery.json

Tap the D-pad once per press you want classified.
"""

import argparse
import json
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

PAD_ADDR = 0x01
MC_ADDR = 0x81
CMD_POLL = 0x42

BUTTONS = {
    "select": 0x0001, "l3": 0x0002, "r3": 0x0004, "start": 0x0008,
    "up": 0x0010, "right": 0x0020, "down": 0x0040, "left": 0x0080,
    "l2": 0x0100, "r2": 0x0200, "l1": 0x0400, "r1": 0x0800,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
}
DPAD = ("up", "right", "down", "left")


def _b(v):
    """sio_trace reports bytes as '0x41' strings."""
    return int(v, 16) if isinstance(v, str) else int(v)


def decode_polls(entries):
    """Reconstruct pad polls from an ordered SIO byte stream.

    `entries` must be ordered by seq and may contain memory-card traffic,
    which is skipped.  Returns one dict per pad transaction that got far
    enough to carry a button word.
    """
    polls = []
    txn = None
    for e in entries:
        tx, rx = _b(e["tx"]), _b(e["rx"])
        if tx in (PAD_ADDR, MC_ADDR):
            # A new address byte always ends the previous transaction.
            if txn is not None:
                polls.append(txn) if txn["complete"] else None
            txn = {"seq": e["seq"], "addr": tx, "bytes": [], "frame": e.get("frame", -1),
                   "cmd": None, "id": None, "buttons": None, "sticks": None,
                   "complete": False} if tx == PAD_ADDR else None
            continue
        if txn is None:
            continue
        txn["bytes"].append(rx)
        n = len(txn["bytes"])
        if n == 1:
            txn["cmd"] = tx      # the command byte we clocked out
            txn["id"] = rx
        elif n == 4:
            txn["buttons"] = txn["bytes"][2] | (txn["bytes"][3] << 8)
            txn["complete"] = txn["cmd"] == CMD_POLL
        elif n == 8:
            txn["sticks"] = [txn["bytes"][6], txn["bytes"][7],
                             txn["bytes"][4], txn["bytes"][5]]  # lx,ly,rx,ry
    if txn is not None and txn["complete"]:
        polls.append(txn)
    return polls


def pressed(word, name):
    return (word & BUTTONS[name]) == 0


def poll_runs(polls, name):
    """Contiguous spans of polls where `name` is held. Returns (i0, i1) pairs."""
    runs, start = [], None
    for i, p in enumerate(polls):
        if p["buttons"] is not None and pressed(p["buttons"], name):
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i - 1))
            start = None
    if start is not None:
        runs.append((start, len(polls) - 1))
    return runs


def analyze(polls, bounce_polls=12):
    """Classify delivered pad polls.

    bounce_polls: two runs separated by fewer than this many polls came from
    one physical tap (a tap is ~4 polls; a deliberate second tap is ~30+).
    """
    runs, findings = [], []
    for name in BUTTONS:
        spans = poll_runs(polls, name)
        for i0, i1 in spans:
            runs.append({
                "button": name, "i0": i0, "i1": i1,
                "polls": i1 - i0 + 1,
                "seq0": polls[i0]["seq"], "seq1": polls[i1]["seq"],
                "frame0": polls[i0].get("frame", -1),
                "frame1": polls[i1].get("frame", -1),
            })
        for (a0, a1), (b0, _b1) in zip(spans, spans[1:]):
            gap = b0 - a1 - 1
            if gap <= bounce_polls:
                findings.append({
                    "kind": "delivered_bounce", "button": name,
                    "gap_polls": gap,
                    "detail": "the game read %s as released for %d poll(s) in "
                              "the middle of one press, then held again — two "
                              "rising edges, two menu moves" % (name, gap),
                })
    runs.sort(key=lambda r: r["i0"])
    return runs, findings


def polls_per_frame(polls):
    """How many times the game read the pad in each frame it polled."""
    counts = {}
    for p in polls:
        f = p.get("frame", -1)
        if f >= 0:
            counts[f] = counts.get(f, 0) + 1
    return counts


def resolve_frame(prev_frame, frame):
    """Exact frame for entries seen between two reads, or -1 if unknowable.

    Entries newly visible in this batch were generated between the previous
    read and this one.  If the frame counter did not advance across that
    interval, every one of them belongs to that frame.  If it did advance,
    they straddle a boundary and this ring carries no per-entry stamp to
    separate them — so say -1 rather than invent one.
    """
    if prev_frame is None or frame < 0 or prev_frame != frame:
        return -1
    return frame


def capture(host, port, seconds, count):
    import pad_trace
    link = pad_trace.Link(host, port)
    seen_max, entries, gaps = -1, [], 0
    prev_frame = None
    t_end = time.time() + seconds
    while time.time() < t_end:
        r = link.cmd_with({"cmd": "sio_trace", "count": count})
        fr = link.cmd_with({"cmd": "frame"})
        frame = int(fr.get("frame", -1)) if fr else -1
        if not r or "entries" not in r:
            raise RuntimeError("sio_trace failed: %r" % (r,))
        batch = r["entries"]
        if not batch:
            continue
        first = batch[0]["seq"]
        if seen_max >= 0 and first > seen_max + 1:
            gaps += first - seen_max - 1
        stamp = resolve_frame(prev_frame, frame)
        prev_frame = frame
        for e in batch:
            if e["seq"] > seen_max:
                e["frame"] = stamp
                entries.append(e)
                seen_max = e["seq"]
    link.close()
    return entries, gaps, link


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--count", type=int, default=192,
                    help="sio_trace entries per poll; raise if gaps appear")
    ap.add_argument("--json")
    args = ap.parse_args()

    print("Capturing %.0fs — tap the D-pad." % args.seconds)
    entries, gaps, link = capture(args.host, args.port, args.seconds, args.count)
    polls = decode_polls(entries)
    print("%d SIO bytes, %d pad polls%s"
          % (len(entries), len(polls),
             "" if not link.persistent else ""))
    if gaps:
        print("WARNING: %d bytes missed between reads — raise --count." % gaps)

    ppf = polls_per_frame(polls)
    unknown = sum(1 for p in polls if p.get("frame", -1) < 0)
    if ppf:
        vals = sorted(ppf.values())
        print("polls per frame: min=%d median=%d max=%d  (%d of %d polls "
              "straddled a batch boundary and are unstamped)"
              % (vals[0], vals[len(vals) // 2], vals[-1], unknown, len(polls)))
    # The pad is polled on a fixed cadence; a changing seq delta is the honest
    # way to see irregular polling, since it needs no frame stamp at all.
    deltas = sorted({polls[i + 1]["seq"] - polls[i]["seq"]
                     for i in range(len(polls) - 1)})
    print("poll seq deltas: %s" % (deltas if len(deltas) < 8
                                   else "%d distinct" % len(deltas)))

    runs, findings = analyze(polls)
    for r in runs:
        print("  %-8s %3d polls  seq %d..%d  frames %s..%s"
              % (r["button"], r["polls"], r["seq0"], r["seq1"],
                 r["frame0"], r["frame1"]))
    print()
    if findings:
        for f in findings:
            print("  [%s] %s: %s" % (f["kind"], f["button"], f["detail"]))
    else:
        print("Every press was delivered as one contiguous run.")
        print("The doubling is not in pad delivery — look at the game's own "
              "edge detection or its second input source.")

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"polls": polls, "runs": runs, "findings": findings,
                       "gaps": gaps}, fh, indent=2)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
