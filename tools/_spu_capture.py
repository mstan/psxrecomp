#!/usr/bin/env python3
"""
SPU boot-chime cross-process capture.

Connects to psx-runtime (4370) and psx-beetle (4380), polls spu_voices on
each port at a fixed cadence over a fixed wall-clock window, and dumps a
joined timeline showing per-voice activation, sample-address progression,
and ENDX latches on both backends side-by-side.

The diff highlights:
  - KON sets that one backend honored but the other didn't
  - Voices Beetle is still voicing (adsr_level > 0) but recomp dropped
  - Voices recomp is voicing but Beetle isn't (would be a model bug)
  - ENDX bits set on Beetle but not recomp (block-end miss)

Usage:
    python tools/_spu_capture.py [duration_seconds] [interval_ms]

Defaults: duration=8s, interval=50ms (covers the full Sony intro).
Both processes must already be running. Both windows render at 59.94 Hz
locked, so 8s ≈ 480 polls × 24 voices on each side.
"""

import json, socket, sys, time
from typing import Optional

PORTS = [("recomp", 4370), ("beetle", 4380)]


class Cli:
    def __init__(self, port: int):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(3.0)
        self.buf = b""
        self.id = 0

    def call(self, cmd: str, **kw) -> Optional[dict]:
        self.id += 1
        msg = {"cmd": cmd, "id": self.id, **kw}
        try:
            self.s.sendall((json.dumps(msg) + "\n").encode())
            while b"\n" not in self.buf:
                chunk = self.s.recv(65536)
                if not chunk:
                    return None
                self.buf += chunk
            line, _, self.buf = self.buf.partition(b"\n")
            return json.loads(line.decode())
        except Exception as e:
            print(f"[port-error] {e}", file=sys.stderr)
            return None


def fmt_mask(mask: int) -> str:
    bits = "".join("1" if (mask >> i) & 1 else "." for i in range(24))
    return bits


def hexish(v):
    if isinstance(v, str) and v.startswith("0x"):
        return int(v, 16)
    return int(v)


def diff_snapshot(rec: dict, bee: dict) -> dict:
    if not rec or not bee:
        return {"err": "missing", "rec_ok": rec is not None, "bee_ok": bee is not None}
    rec_active = hexish(rec["active_mask"])
    bee_active = hexish(bee["active_mask"])
    rec_endx   = hexish(rec.get("endx", "0x0"))
    bee_kon    = hexish(bee.get("kon", "0x0"))   # voice_on (latch)
    bee_koff   = hexish(bee.get("koff", "0x0"))  # voice_off
    rec_kon    = hexish(rec.get("kon", "0x0"))
    rec_koff   = hexish(rec.get("koff", "0x0"))

    only_rec = rec_active & ~bee_active
    only_bee = bee_active & ~rec_active
    both     = rec_active &  bee_active

    return {
        "rec_active": rec_active,
        "bee_active": bee_active,
        "only_rec":   only_rec,
        "only_bee":   only_bee,
        "both":       both,
        "rec_kon":    rec_kon,
        "bee_kon":    bee_kon,
        "rec_koff":   rec_koff,
        "bee_koff":   bee_koff,
        "rec_endx":   rec_endx,
    }


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    interval = float(sys.argv[2]) / 1000.0 if len(sys.argv) > 2 else 0.050

    print(f"[capture] duration={duration}s interval={interval*1000:.0f}ms")

    clis = {}
    for name, port in PORTS:
        try:
            clis[name] = Cli(port)
            ping = clis[name].call("ping")
            print(f"[{name}:{port}] ping = {ping}")
        except Exception as e:
            print(f"[{name}:{port}] connect failed: {e}", file=sys.stderr)
            return 1

    snapshots = []
    t0 = time.time()
    while time.time() - t0 < duration:
        snap = {"t": time.time() - t0}
        for name in clis:
            r = clis[name].call("spu_voices")
            snap[name] = r if r and r.get("ok") else None
        snapshots.append(snap)
        time.sleep(interval)

    print(f"[capture] {len(snapshots)} snapshots collected")

    # First and last non-null on each side
    print("\n=== TIMELINE: voice activity (24 bits, MSB=v23) ===")
    print(f"  {'t(s)':>6}  recomp{' '*18}  beetle{' '*18}  drift")
    last_rec = last_bee = 0
    for snap in snapshots:
        d = diff_snapshot(snap.get("recomp"), snap.get("beetle"))
        if "err" in d: continue
        rec = d["rec_active"]
        bee = d["bee_active"]
        if rec != last_rec or bee != last_bee:
            mark = ""
            if d["only_bee"] and not d["only_rec"]:
                mark = f" beetle-only=0x{d['only_bee']:06X}"
            elif d["only_rec"] and not d["only_bee"]:
                mark = f" recomp-only=0x{d['only_rec']:06X}"
            elif d["only_bee"] and d["only_rec"]:
                mark = f" rec-only=0x{d['only_rec']:06X} bee-only=0x{d['only_bee']:06X}"
            print(f"  {snap['t']:6.2f}  {fmt_mask(rec)}  {fmt_mask(bee)} {mark}")
            last_rec, last_bee = rec, bee

    # KON/KOFF summary
    print("\n=== KEYON / KEYOFF latches (most recent) ===")
    if snapshots:
        last = snapshots[-1]
        if last.get("recomp"):
            print(f"  recomp: kon=0x{hexish(last['recomp'].get('kon','0x0')):06X}  "
                  f"koff=0x{hexish(last['recomp'].get('koff','0x0')):06X}  "
                  f"endx=0x{hexish(last['recomp'].get('endx','0x0')):06X}")
        if last.get("beetle"):
            print(f"  beetle: kon=0x{hexish(last['beetle'].get('kon','0x0')):06X}  "
                  f"koff=0x{hexish(last['beetle'].get('koff','0x0')):06X}  "
                  f"endx=0x{hexish(last['beetle'].get('endx','0x0')):06X}")

    # Per-voice end-state
    print("\n=== END STATE (last snapshot, per voice) ===")
    print("                  recomp                          beetle")
    print("  v   active  cur_addr  flags  s_idx     active  cur_addr  adsr_level  pitch_b")
    if snapshots:
        last = snapshots[-1]
        rec = last.get("recomp")
        bee = last.get("beetle")
        rec_voices = {v["v"]: v for v in (rec.get("voices", []) if rec else [])}
        bee_voices = {v["v"]: v for v in (bee.get("voices", []) if bee else [])}
        for v in range(24):
            r = rec_voices.get(v, {})
            b = bee_voices.get(v, {})
            print(f"  {v:2d}  "
                  f"{r.get('active','?'):>6}  "
                  f"{r.get('cur_addr','?'):>8}  "
                  f"{r.get('flags','?'):>5}  "
                  f"{r.get('sample_idx','?'):>5}     "
                  f"{b.get('active','?'):>6}  "
                  f"{b.get('cur_addr','?'):>8}  "
                  f"{b.get('adsr_level','?'):>10}  "
                  f"{b.get('pitch','?'):>7}")

    # SPU events from recomp
    print("\n=== RECOMP SPU EVENT TRAIL (most recent 200) ===")
    r = clis["recomp"].call("spu_events", count=200)
    if r and r.get("ok"):
        evs = r.get("events", [])
        print(f"  total={r['total']} returned={len(evs)}")
        for e in evs:
            print(f"  seq={e['seq']:>5}  f={e['frame']:>5}  v={e['v']:>2}  "
                  f"{e['kind']:<10}  addr={e['addr']}  pitch={e['pitch']}  "
                  f"adsr={e['adsr_lo']}/{e['adsr_hi']}  vol={e['vol_l']}/{e['vol_r']}")
    else:
        print("  (spu_events query failed)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
