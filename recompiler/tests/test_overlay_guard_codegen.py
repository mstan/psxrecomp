#!/usr/bin/env python3
"""Recompiler codegen regression test for the native-overlay blue-screen wedge.

Deterministic integration test (no game, no capture): synthesize a tiny PS-EXE
with a function that makes a call (so its CPS entry-switch carries a continuation),
run psxrecomp-game in BOTH --overlay and static mode, and assert:

  OVERLAY mode  -> the function's entry-switch FAILS CLOSED on an unknown entry:
                   it emits `psx_native_bad_entry(` in the `default:` case instead
                   of falling through and running the function from its top. This
                   is the fix for the foreign-interior-entry wedge (a native overlay
                   fn entered at a PC it doesn't own must bail to the interpreter,
                   not run from the top and corrupt shared CPU/RAM state).

  STATIC mode   -> keeps the legacy `default: break;` (no psx_native_bad_entry):
                   static ranges are discovered once with no multi-variant entry
                   ambiguity, so the guard is overlay-scoped (config_.overlay_mode).

Usage:  python test_overlay_guard_codegen.py [--recompiler <psxrecomp-game.exe>]
Exit 0 = PASS.
"""
import argparse, os, struct, subprocess, sys, tempfile

LOAD = 0x80010000


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)        # initial PC
    struct.pack_into("<I", h, 0x18, LOAD)         # load address
    struct.pack_into("<I", h, 0x1C, len(data))    # text size
    return bytes(h) + data


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def build_exe():
    # func A @ 0x80010000: addiu sp,-8 ; jal B ; nop(delay) ; addiu sp,8 ; jr ra ; nop
    #   -> the return point after the jal is a CPS continuation, so A gets an
    #      entry-switch (where the fail-closed default lives).
    # func B @ 0x80010020: jr ra ; nop
    a = [0x27BDFFF8, jal(0x80010020), 0x00000000, 0x27BD0008, 0x03E00008, 0x00000000]
    body = bytearray(w(a))
    body += b"\x00" * (0x20 - len(body))          # pad to B's offset
    body += w([0x03E00008, 0x00000000])           # func B
    return make_psxexe(LOAD, bytes(body))


def gen_c(recompiler, overlay, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x80010000\n0x80010020\n")
    cmd = [recompiler, psx, "--seeds", seeds, "--out-dir", out]
    if overlay:
        cmd.append("--overlay")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"recompiler failed ({'overlay' if overlay else 'static'}):\n"
                         f"{r.stderr or r.stdout}")
    # Overlay mode emits one *_full.c; static mode may shard it as
    # *_full_00.c, *_full_01.c, ... . Inspect the complete generated body in
    # either representation, excluding the dispatch TU.
    full = sorted(f for f in os.listdir(out)
                  if "_full" in f and f.endswith(".c") and "_dispatch" not in f)
    if not full:
        raise SystemExit(f"no _full*.c emitted in {out}")
    chunks = []
    for name in full:
        with open(os.path.join(out, name)) as f:
            chunks.append(f.read())
    return "\n".join(chunks)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_recomp = os.path.normpath(os.path.join(
        here, "..", "build", "psxrecomp-game.exe"))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", default=default_recomp)
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler} (build it first)")

    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        ov = gen_c(args.recompiler, True, tmp)
        st = gen_c(args.recompiler, False, tmp)

    if "psx_native_bad_entry(" not in ov:
        fails.append("OVERLAY codegen is MISSING the fail-closed guard "
                     "(no psx_native_bad_entry in the entry-switch default) — the "
                     "foreign-interior-entry wedge would run the fn from its top.")
    else:
        print("PASS: overlay codegen emits the fail-closed guard (psx_native_bad_entry).")

    if "psx_native_bad_entry(" in st:
        fails.append("STATIC codegen unexpectedly contains psx_native_bad_entry — "
                     "the guard must be overlay-scoped (config_.overlay_mode).")
    else:
        print("PASS: static codegen keeps the legacy fall-through (no guard).")

    if fails:
        for m in fails:
            print("FAIL:", m)
        return 1
    print("PASS: native-overlay fail-closed guard codegen is correct.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
