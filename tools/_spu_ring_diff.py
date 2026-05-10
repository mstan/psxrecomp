#!/usr/bin/env python3
"""
SPU event-ring diff. Both backends maintain an always-on ring buffer of
KEYON / KEYOFF / END_STOP / END_LOOP events, frame-stamped by absolute
simulated frame count. This script dumps both rings AFTER the chime is
done playing and aligns events to find divergences.

Usage:
    python tools/_spu_ring_diff.py [count]    # default 1000

Run AFTER both backends have played through the boot chime fully.
"""

import json, socket, sys
from collections import defaultdict
from typing import Optional


class Cli:
    def __init__(self, port: int):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.s.settimeout(5.0)
        self.buf = b""
        self.id = 0
        self.port = port

    def call(self, cmd: str, **kw) -> Optional[dict]:
        self.id += 1
        msg = {"cmd": cmd, "id": self.id, **kw}
        self.s.sendall((json.dumps(msg) + "\n").encode())
        # Both backends emit newline-terminated JSON. Read until we see one,
        # then parse. If parse fails on the first line, surface why.
        # Read until we see a newline or hit a hard byte cap; the socket
        # timeout is the real deadline. TCP loopback can fragment a 200KB
        # JSON object into hundreds of small reads.
        BYTE_CAP = 16 * 1024 * 1024
        while True:
            if b"\n" in self.buf:
                line, _, self.buf = self.buf.partition(b"\n")
                try:
                    return json.loads(line.decode())
                except Exception as e:
                    print(f"[err] port {self.port} parse failed: {e}; "
                          f"line_len={len(line)} tail={line[-120:]!r}",
                          file=sys.stderr)
                    return None
            try:
                chunk = self.s.recv(1 << 20)
            except socket.timeout:
                print(f"[err] port {self.port} recv timeout after "
                      f"{len(self.buf)} bytes buffered", file=sys.stderr)
                return None
            if not chunk:
                print(f"[err] port {self.port} EOF after {len(self.buf)} bytes",
                      file=sys.stderr)
                return None
            self.buf += chunk
            if len(self.buf) > BYTE_CAP:
                print(f"[err] port {self.port} response exceeds {BYTE_CAP} bytes "
                      f"without newline", file=sys.stderr)
                return None


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 1000

    rec = Cli(4370)
    bee = Cli(4380)

    r_evs = rec.call("spu_events", count=count)
    b_evs = bee.call("spu_events", count=count)

    if not (r_evs and r_evs.get("ok")):
        print(f"[err] recomp spu_events failed: {r_evs}", file=sys.stderr)
        return 1
    if not (b_evs and b_evs.get("ok")):
        print(f"[err] beetle spu_events failed: {b_evs}", file=sys.stderr)
        return 1

    r_list = r_evs["events"]
    b_list = b_evs["events"]
    print(f"[recomp] total={r_evs['total']} returned={len(r_list)} "
          f"frames={r_list[0]['frame'] if r_list else '-'}..{r_list[-1]['frame'] if r_list else '-'}")
    print(f"[beetle] total={b_evs['total']} returned={len(b_list)} "
          f"frames={b_list[0]['frame'] if b_list else '-'}..{b_list[-1]['frame'] if b_list else '-'}")

    # Find frame-offset between the two by aligning the first KEYON of each.
    r_first_kon = next((e for e in r_list if e["kind"] == "KEYON"), None)
    b_first_kon = next((e for e in b_list if e["kind"] == "KEYON"), None)
    if not (r_first_kon and b_first_kon):
        print("[err] no KEYON events found")
        return 1
    delta = r_first_kon["frame"] - b_first_kon["frame"]
    print(f"\n[align] first KEYON: rec.frame={r_first_kon['frame']} "
          f"bee.frame={b_first_kon['frame']}  delta_rec_minus_bee={delta}")

    # Emit aligned timeline: bee frame → list of events. Add rec events
    # offset by -delta (so rec frames align with bee frames).
    print("\n=== ALIGNED TIMELINE (bee_frame: rec_event | bee_event) ===")
    print("'r/b': what each backend records at the equivalent frame.")
    print("ENV col is post-event envelope level (0..0x7FFF).\n")

    # Group events per bee-frame.
    by_bf = defaultdict(lambda: {"r": [], "b": []})
    for e in r_list:
        bf = e["frame"] - delta
        by_bf[bf]["r"].append(e)
    for e in b_list:
        by_bf[e["frame"]]["b"].append(e)

    last_summary = None
    for bf in sorted(by_bf):
        slot = by_bf[bf]
        r_kinds = sorted((e["kind"], e["v"]) for e in slot["r"])
        b_kinds = sorted((e["kind"], e["v"]) for e in slot["b"])
        if r_kinds == b_kinds: continue  # match, skip
        # Print divergence
        rstr = ",".join(f"{k}.v{v}" for k, v in r_kinds) or "-"
        bstr = ",".join(f"{k}.v{v}" for k, v in b_kinds) or "-"
        only_r = set(r_kinds) - set(b_kinds)
        only_b = set(b_kinds) - set(r_kinds)
        marker = ""
        if only_r and not only_b: marker = "  &lt;- rec-extra"
        elif only_b and not only_r: marker = "  &lt;- rec-missing"
        elif only_r and only_b:    marker = "  &lt;- divergent"
        print(f"  bf=+{bf-b_first_kon['frame']:>4}  r=[{rstr}]  b=[{bstr}]{marker}")

    # Per-voice keyon/keyoff count summary
    print("\n=== PER-VOICE EVENT COUNTS ===")
    print("  v   rec_kon  bee_kon  rec_koff  bee_koff  rec_endloop  bee_endloop  rec_endstop  bee_endstop")
    counts = {v: defaultdict(int) for v in range(24)}
    for e in r_list: counts[e["v"]][f"r_{e['kind']}"] += 1
    for e in b_list: counts[e["v"]][f"b_{e['kind']}"] += 1
    for v in range(24):
        c = counts[v]
        if not any(c.values()): continue
        print(f"  {v:2d}  "
              f"{c['r_KEYON']:>7}  {c['b_KEYON']:>7}  "
              f"{c['r_KEYOFF']:>8}  {c['b_KEYOFF']:>8}  "
              f"{c['r_END_LOOP']:>11}  {c['b_END_LOOP']:>11}  "
              f"{c['r_END_STOP']:>11}  {c['b_END_STOP']:>11}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
