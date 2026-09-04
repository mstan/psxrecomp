#!/usr/bin/env python3
"""
menu_scan.py — Find the menu cursor variable, then prove how far it moves.

The reported bug is that one D-pad tap moves the menu cursor twice.  Both the
host pad state and the delivered SIO polls have been measured clean, so the
next question is what the *game* does with the input.  This finds the RAM
location holding the cursor index by snapshotting main RAM while you tap, and
keeping only the locations whose value changes when — and only when — a tap
happened, by a small step.

Taps are located exactly, using the SIO byte ring rather than the snapshot
sampling rate: a 100 ms tap falls between two 250 ms snapshots and would be
invisible otherwise.  Each snapshot records the ring's sequence counter, and
the taps decoded from the ring afterwards are placed into the interval they
fall in.

Usage:
    python3 menu_scan.py --snapshots 40 --interval 0.25 --json /tmp/menu.json

Sit in the menu and tap the SAME direction several times, evenly spaced.
"""

import argparse
import json
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

RAM_BASE = 0x80000000
RAM_SIZE = 0x200000
CHUNK = 0x40000
BLOCK = 1024


def find_changed_offsets(a, b, block=BLOCK):
    """Byte offsets where two snapshots differ.

    Compares block-at-a-time first: in a static menu almost all of RAM is
    unchanged, so this skips the vast majority at C speed and only walks
    bytes inside blocks that actually moved.
    """
    out = []
    n = min(len(a), len(b))
    for base in range(0, n, block):
        end = min(base + block, n)
        if a[base:end] == b[base:end]:
            continue
        for i in range(base, end):
            if a[i] != b[i]:
                out.append(i)
    return out


def held_spans(entries, button):
    """Ring-sequence spans during which `button` was held.

    Rising edges alone are the wrong model: holding a direction makes the
    game auto-repeat, so the cursor moves repeatedly with no new edge.
    Treating those moves as "movement without input" disqualifies the very
    variable being hunted, so the whole held period counts as input.
    """
    import pad_delivery
    polls = pad_delivery.decode_polls(entries)
    spans, start, last = [], None, None
    for p in polls:
        if p["buttons"] is None:
            continue
        down = pad_delivery.pressed(p["buttons"], button)
        if down and start is None:
            start = p["seq"]
        elif not down and start is not None:
            spans.append((start, last))
            start = None
        last = p["seq"]
    if start is not None:
        spans.append((start, last))
    return spans


def input_intervals(before, after, spans_by_button, settle=1):
    """Intervals where movement is legitimate, and which press caused each.

    Every direction must be supplied, not just the one under study: a cursor
    moves on `up` as well as `down`, and treating an unmonitored direction as
    "no input" disqualifies the cursor itself.

    A change seen between snapshots i and i+1 was caused by input somewhere
    between the start of snapshot i's read and the end of snapshot i+1's --
    reads take real time, so that window is the honest bound.

    Returns (allowed, presses) where presses is a list of
    (button, first_interval, last_interval) in capture order.
    """
    allowed, presses = set(), []
    n = len(before)
    for button, spans in sorted(spans_by_button.items()):
        for s0, s1 in spans:
            touched = []
            for i in range(n - 1):
                if s0 < after[i + 1] and s1 >= before[i]:
                    touched.append(i)
            if not touched:
                continue
            lo, hi = touched[0], touched[-1]
            for i in range(lo, min(hi + settle, n - 2) + 1):
                allowed.add(i)
            presses.append((button, lo, min(hi + settle, n - 2)))
    presses.sort(key=lambda p: p[1])
    return allowed, presses


def press_deltas(values, presses, n):
    """Net movement across each press, measured settled-to-settled.

    An animated cursor slides into place over several frames, so a per-interval
    delta samples the animation rather than the step.  Comparing the value
    before the press with the value once it has settled measures the step the
    game actually took.
    """
    out = []
    for button, lo, hi in presses:
        a = values[lo]
        b = values[min(hi + 1, n - 1)]
        out.append((button, byte_delta(a, b)))
    return out


def byte_delta(a, b):
    """Signed step from byte value a to b, accounting for 8-bit wrap."""
    d = b - a
    if d > 128:
        d -= 256
    elif d < -128:
        d += 256
    return d


def classify_offset(values, allowed, presses, max_multiple=2, min_moves=3):
    """Does this location behave like a cursor?

    Disqualified if it ever moves with no input.  Otherwise judged on the
    net step per press, measured settled-to-settled:

      * each direction must move it consistently one way -- `up` and `down`
        move a cursor opposite ways, so a single global sign test would
        reject the very thing we are looking for;
      * every step must be one stride or exactly two, with the stride taken
        from the data rather than assumed;
      * it must respond to several presses, since one press cannot establish
        a stride and coincidences are cheap.
    """
    n = len(values)
    for i in range(n - 1):
        if values[i] != values[i + 1] and i not in allowed:
            return None                       # moved with no input

    per = press_deltas(values, presses, n)
    moves = [(b, d) for b, d in per if d != 0]
    if not moves:
        return None

    by_button = {}
    for b, d in moves:
        by_button.setdefault(b, []).append(d)
    consistent = all(all(x > 0 for x in v) or all(x < 0 for x in v)
                     for v in by_button.values())

    base = min(abs(d) for _, d in moves)
    regular = base > 0 and all(abs(d) % base == 0 and abs(d) // base <= max_multiple
                               for _, d in moves)
    # Opposite directions moving opposite ways is the signature of an axis.
    opposed = False
    for a, b in (("up", "down"), ("left", "right")):
        if a in by_button and b in by_button:
            if by_button[a][0] * by_button[b][0] < 0:
                opposed = True

    strict = consistent and regular and len(moves) >= min_moves
    return {"moves": len(moves), "per_press": per, "base": base,
            "strict": strict, "values": list(values), "opposed": opposed,
            "deltas": [d for _, d in moves], "consistent": consistent,
            "doubled": strict and any(abs(d) // base == 2 for _, d in moves)}


def decode_taps(entries, button):
    """Ring sequence of each rising edge of `button` in the SIO stream."""
    import pad_delivery
    polls = pad_delivery.decode_polls(entries)
    taps, held = [], False
    for p in polls:
        if p["buttons"] is None:
            continue
        now = pad_delivery.pressed(p["buttons"], button)
        if now and not held:
            taps.append(p["seq"])
        held = now
    return taps


def read_ram(link, base, size):
    out = bytearray()
    for off in range(0, size, CHUNK):
        n = min(CHUNK, size - off)
        r = link.cmd_with({"cmd": "read_ram",
                           "addr": "0x%08X" % (base + off), "len": n})
        if not r or not r.get("ok"):
            raise RuntimeError("read_ram failed at 0x%08X: %r" % (base + off, r))
        out.extend(bytes.fromhex(r["hex"]))
    return bytes(out)


def capture(host, port, snapshots, interval, ring_count=512):
    """Snapshot RAM repeatedly, stitching the SIO ring as we go.

    The ring is pulled every round and merged by sequence number.  Pulling
    once at the end instead would silently lose any tap that scrolled out of
    the window, and a lost tap does not merely go unseen -- it disqualifies
    the very variable we are looking for, because that variable then appears
    to move with no input.
    """
    import pad_trace
    link = pad_trace.Link(host, port)
    snaps, before, after, ring, seen_max, gaps = [], [], [], [], -1, 0
    for _ in range(snapshots):
        t0 = time.time()
        r = link.cmd_with({"cmd": "sio_trace", "count": ring_count})
        before.append(int(r["total"]) if r and "total" in r else -1)
        for e in (r or {}).get("entries", []):
            if e["seq"] > seen_max:
                if seen_max >= 0 and e["seq"] > seen_max + 1 and not ring:
                    pass
                ring.append(e)
                seen_max = e["seq"]
        snaps.append(read_ram(link, RAM_BASE, RAM_SIZE))
        r2 = link.cmd_with({"cmd": "sio_trace", "count": ring_count})
        after.append(int(r2["total"]) if r2 and "total" in r2 else -1)
        for e in (r2 or {}).get("entries", []):
            if e["seq"] > seen_max:
                ring.append(e)
                seen_max = e["seq"]
        spent = time.time() - t0
        if interval > spent:
            time.sleep(interval - spent)
    r = link.cmd_with({"cmd": "sio_trace", "count": ring_count})
    for e in (r or {}).get("entries", []):
        if e["seq"] > seen_max:
            ring.append(e)
            seen_max = e["seq"]
    link.close()
    ring.sort(key=lambda e: e["seq"])
    for i in range(1, len(ring)):
        if ring[i]["seq"] != ring[i - 1]["seq"] + 1:
            gaps += ring[i]["seq"] - ring[i - 1]["seq"] - 1
    return snaps, before, after, ring, gaps


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--snapshots", type=int, default=40)
    ap.add_argument("--interval", type=float, default=0.25)
    ap.add_argument("--buttons", default="up,down,left,right",
                    help="all directions that should count as input")
    ap.add_argument("--max-report", type=int, default=40)
    ap.add_argument("--min-moves", type=int, default=3,
                    help="presses a location must respond to")
    ap.add_argument("--settle", type=int, default=2,
                    help="intervals after a tap in which movement is still "
                         "allowed (animated cursors)")
    ap.add_argument("--json")
    args = ap.parse_args()

    print("Capturing %d snapshots %.2fs apart (%.0fs) — sit in the menu and "
          "tap %s several times."
          % (args.snapshots, args.interval,
             args.snapshots * args.interval, args.buttons))
    snaps, before, after, ring, gaps = capture(
        args.host, args.port, args.snapshots, args.interval)
    if gaps:
        print("WARNING: %d SIO bytes missed; a tap may be undetected." % gaps)
    buttons = [b.strip() for b in args.buttons.split(",") if b.strip()]
    spans_by_button = {b: held_spans(ring, b) for b in buttons}
    tapped, presses = input_intervals(before, after, spans_by_button,
                                      settle=args.settle)
    counts = ", ".join("%s=%d" % (b, len(spans_by_button[b])) for b in buttons)
    print("%d snapshots, presses in ring: %s -> %d located, %d interval(s) "
          "where movement is allowed"
          % (len(snaps), counts, len(presses), len(tapped)))
    located = len(presses)
    if not tapped:
        print("No taps located inside the capture window — nothing to "
              "correlate against. Tap during the capture and retry.")
        return 1

    # Union of everything that ever moved, then test each against the taps.
    moved = set()
    for i in range(len(snaps) - 1):
        moved.update(find_changed_offsets(snaps[i], snaps[i + 1]))
    print("%d bytes changed at least once" % len(moved))

    strict, loose = [], []
    for off in sorted(moved):
        vals = [s_[off] for s_ in snaps]
        sc = classify_offset(vals, tapped, presses,
                             min_moves=args.min_moves)
        if not sc:
            continue
        sc["addr"] = RAM_BASE + off
        (strict if sc["strict"] else loose).append(sc)
    strict.sort(key=lambda h: (-h["moves"], not h["opposed"]))
    loose.sort(key=lambda h: -h["moves"])

    def show(rows, label):
        print("\n%d %s:" % (len(rows), label))
        for h in rows[:args.max_report]:
            flag = "  <-- DOUBLE STEP" if h.get("doubled") else ""
            if h.get("opposed"):
                flag += "  [axis]"
            per = " ".join("%s:%+d" % (b, d)
                           for b, d in h.get("per_press", []) if d)
            print("  0x%08X  moves=%d/%d base=%d  [%s]%s"
                  % (h["addr"], h["moves"], located, h["base"], per, flag))
        if len(rows) > args.max_report:
            print("  ... %d more (raise --max-report)"
                  % (len(rows) - args.max_report))

    show(strict, "location(s) stepping consistently on presses")
    if not strict:
        show(loose, "location(s) that moved only on taps, but irregularly")
    hits = strict or loose

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"presses": presses, "allowed": sorted(tapped),
                       "strict": strict, "loose": loose}, fh, indent=2)
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
