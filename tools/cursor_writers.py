#!/usr/bin/env python3
"""
cursor_writers.py — Catch every write to the menu cursor and name the writer.

menu_scan.py located a variable that moved two steps from one D-pad tap.
This registers a write-trace range over it and records each write during a
capture: the value before and after, the PC that made it, and the frame.

That distinguishes the two ways one tap becomes two moves:

  two writes, two PCs   two independent code paths each acted on the input.
                        If one reads the button bits and the other the stick
                        bytes, the D-pad/stick fold is implicated.
  two writes, one PC    one path ran twice — the game polled or ticked twice.
  one write of -2       one path computed a step of two; the doubling is in
                        the step calculation, and input is not to blame.

Usage:
    python3 cursor_writers.py --seconds 12 --focus 0x8004A5F5

Registers a trace range, captures, then always removes the range it added.
"""

import argparse
import json
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

PHYS_MASK = 0x1FFFFFFF


def phys(addr):
    """The trace ring records physical addresses; KSEG0/KSEG1 map down."""
    return addr & PHYS_MASK


def writes_in_range(entries, lo, hi):
    """Trace entries whose address falls in [lo, hi), oldest first."""
    out = []
    for e in entries:
        a = int(e["addr"], 16)
        if phys(lo) <= a < phys(hi):
            out.append(e)
    out.sort(key=lambda e: e["seq"])
    return out


def covers(e, addr):
    """Does this write touch `addr`?

    The ring records a write at its base address and width, so a halfword
    store to 0x...F4 changes the byte at 0x...F5 without ever appearing at
    that address.  Matching on address equality misses those entirely.
    """
    a = int(e["addr"], 16)
    return a <= phys(addr) < a + e.get("w", 4)


def byte_value(e, key, addr):
    """The byte this write put at `addr` ("old" or "new")."""
    shift = (phys(addr) - int(e["addr"], 16)) * 8
    return (int(e[key], 16) >> shift) & 0xFF


def byte_delta(e, addr):
    """Signed change this write made to the single byte at `addr`."""
    d = byte_value(e, "new", addr) - byte_value(e, "old", addr)
    if d > 128:
        d -= 256
    elif d < -128:
        d += 256
    return d


def touching_writes(writes, addr):
    """Writes that actually changed the byte at `addr`.

    A wide store may cover the byte while leaving it alone; that is not a
    cursor move and must not be counted as one.
    """
    return [e for e in writes if covers(e, addr) and byte_delta(e, addr) != 0]


def delta(e):
    """Signed change this write made, at the traced width."""
    w = e.get("w", 4)
    bits = w * 8
    old = int(e["old"], 16) & ((1 << bits) - 1)
    new = int(e["new"], 16) & ((1 << bits) - 1)
    d = new - old
    # Interpret a large jump as a small signed step (wrap at the width).
    if d > (1 << (bits - 1)):
        d -= (1 << bits)
    elif d < -(1 << (bits - 1)):
        d += (1 << bits)
    return d


def summarize(writes, focus):
    """Per-address rollup, with the focus address reported in full."""
    by_addr = {}
    for e in writes:
        a = int(e["addr"], 16)
        s = by_addr.setdefault(a, {"addr": a, "writes": 0, "pcs": set(),
                                   "funcs": set(), "deltas": []})
        s["writes"] += 1
        s["pcs"].add(e["pc"])
        s["funcs"].add(e.get("func", "?"))
        s["deltas"].append(delta(e))
    for s in by_addr.values():
        s["pcs"] = sorted(s["pcs"])
        s["funcs"] = sorted(s["funcs"])
    focus_writes = touching_writes(writes, focus)
    return by_addr, focus_writes


def group_events(writes, focus, gap_frames=8):
    """Split writes into one group per press.

    A press produces its writes within a frame or two; separate presses are
    seconds apart.  Grouping keeps a capture containing several presses from
    being read as one confused event.
    """
    events, cur = [], []
    for e in sorted(writes, key=lambda x: x["seq"]):
        if cur and (e.get("frame", 0) - cur[-1].get("frame", 0)) > gap_frames:
            events.append(cur)
            cur = []
        cur.append(e)
    if cur:
        events.append(cur)
    return events


def describe_event(ev, focus):
    total = sum(byte_delta(e, focus) for e in ev)
    pcs = sorted({e["pc"] for e in ev})
    if len(ev) == 1:
        kind = ("single_write_double_step" if abs(total) >= 2
                else "single_write_single_step")
    elif len(pcs) > 1:
        kind = "two_paths"
    else:
        kind = "one_path_twice"
    return {"kind": kind, "writes": len(ev), "total": total, "pcs": pcs,
            "frame0": ev[0].get("frame"), "frame1": ev[-1].get("frame"),
            "funcs": sorted({e.get("func", "?") for e in ev})}


def verdict(focus_writes, focus=None):
    """Name the mechanism, per press and overall."""
    if not focus_writes:
        return ("no_writes",
                "the focus byte was never changed during the capture")
    events = [describe_event(ev, focus)
              for ev in group_events(focus_writes, focus)]
    doubled = [e for e in events if abs(e["total"]) >= 2]
    if not doubled:
        return ("no_double_step",
                "%d press(es) seen, none moved the cursor more than one step"
                % len(events))
    kinds = {e["kind"] for e in doubled}
    if kinds == {"single_write_double_step"}:
        return ("single_write_double_step",
                "%d of %d press(es) moved the cursor %s in ONE write — the "
                "step size was computed wrong; the input path is not at fault"
                % (len(doubled), len(events),
                   "/".join(str(e["total"]) for e in doubled)))
    if "two_paths" in kinds:
        pcs = sorted({pc for e in doubled if e["kind"] == "two_paths"
                      for pc in e["pcs"]})
        return ("two_paths",
                "%d of %d press(es) were acted on by MULTIPLE code paths "
                "(PCs: %s) — two handlers consumed one press"
                % (len(doubled), len(events), ", ".join(pcs)))
    return ("one_path_twice",
            "%d of %d press(es) ran the same handler twice"
            % (len(doubled), len(events)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4370)
    ap.add_argument("--lo", default="0x8004A500")
    ap.add_argument("--hi", default="0x8004A600")
    ap.add_argument("--focus", default="0x8004A5F5")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--count", type=int, default=4096)
    ap.add_argument("--json")
    args = ap.parse_args()

    lo, hi, focus = int(args.lo, 16), int(args.hi, 16), int(args.focus, 16)

    import pad_trace
    link = pad_trace.Link(args.host, args.port)
    slot = None
    try:
        r = link.cmd_with({"cmd": "wtrace_add",
                           "lo": "0x%08X" % lo, "hi": "0x%08X" % hi})
        if not r or not r.get("ok"):
            raise RuntimeError("wtrace_add failed: %r" % (r,))
        slot = r["slot"]
        print("tracing 0x%08X..0x%08X (slot %d) for %.0fs — tap the D-pad "
              "ONCE, and note whether it doubles."
              % (lo, hi, slot, args.seconds))
        time.sleep(args.seconds)
        d = link.cmd_with({"cmd": "wtrace_dump", "addr_lo": "0x%08X" % lo,
                           "addr_hi": "0x%08X" % hi, "count": args.count,
                           "newest": 1})
        entries = (d or {}).get("entries", [])
    finally:
        if slot is not None:
            link.cmd_with({"cmd": "wtrace_del", "slot": slot})
        link.close()

    writes = writes_in_range(entries, lo, hi)
    print("\n%d write(s) in range during the capture" % len(writes))
    by_addr, focus_writes = summarize(writes, focus)

    for a in sorted(by_addr):
        s = by_addr[a]
        print("  0x%08X  writes=%-3d deltas=%-18s pc=%s"
              % (0x80000000 | a, s["writes"],
                 ",".join(str(x) for x in s["deltas"][:6]),
                 ",".join(s["pcs"][:3])))

    print("\nfocus 0x%08X:" % focus)
    for e in focus_writes:
        print("  frame=%-8s @0x%08X w=%s  %s -> %s   byte %d -> %d (%+d)"
              "  pc=%s func=%s ra=%s"
              % (e.get("frame"), int(e["addr"], 16), e.get("w"),
                 e["old"], e["new"], byte_value(e, "old", focus),
                 byte_value(e, "new", focus), byte_delta(e, focus),
                 e["pc"], e.get("func"), e.get("ra")))

    print("\nper-press breakdown:")
    for ev in group_events(focus_writes, focus):
        d = describe_event(ev, focus)
        print("  frames %s..%s  writes=%d  step=%+d  %s  pc=%s"
              % (d["frame0"], d["frame1"], d["writes"], d["total"],
                 d["kind"], ",".join(d["pcs"])))

    kind, text = verdict(focus_writes, focus)
    print("\n[%s] %s" % (kind, text))

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"writes": writes, "verdict": kind, "detail": text},
                      fh, indent=2)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
