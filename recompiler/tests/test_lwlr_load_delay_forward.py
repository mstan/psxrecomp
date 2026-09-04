#!/usr/bin/env python3
"""Recompiler codegen regression test: a pending load forwards into LWL/LWR.

LWL/LWR take their destination register as BOTH the merge base and the
destination, and they read that base late enough to receive the forwarded
result of an immediately preceding load. MIPS-I therefore exempts them from
the load delay: after

    lw   $t0, 12($a3)
    lwr  $t0, 10($a3)

the LWR merges into the word the LW just fetched, not the stale $t0. The
interpreter already models this (dirty_ram_interp.c, the s_ld_pend LWL/LWR
exemption -- without it Tomba 2 wedged at boot), so an emitter that does not
forward puts native and interpreted execution in disagreement.

The load-delay pair emitter defers a load's writeback into psx_ldd_<addr>.
Before the fix the LWL/LWR successor merged cpu->gpr[rt], which still held the
pre-load value, AND the deferred writeback was then discarded because the
successor architecturally writes rt -- so the loaded word vanished entirely.

Asserts the merge operand names the deferred temp and that no discard is
emitted for that site.

Usage:  python test_lwlr_load_delay_forward.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, glob, os, re, struct, subprocess, sys, tempfile

LOAD = 0x80010000

# lw   $t0, 12($a3)   opcode 0x23 rs=7 rt=8 imm=0x000C
LW_T0_A3_12 = 0x8CE8000C
# lwr  $t0, 10($a3)   opcode 0x26 rs=7 rt=8 imm=0x000A
LWR_T0_A3_10 = 0x98E8000A
JR_RA = 0x03E00008
NOP = 0x00000000


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)
    return bytes(h) + data


def build_exe():
    # One function: the dependent lw / lwr pair, then return.
    return make_psxexe(LOAD, LOAD, w([LW_T0_A3_12, LWR_T0_A3_10, JR_RA, NOP]))


def generate(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x%08X\n" % LOAD)
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    srcs = [p for p in glob.glob(os.path.join(out, "*_full*.c"))]
    if not srcs:
        raise SystemExit("no *_full*.c emitted in " + out)
    return "".join(open(p, encoding="utf-8", errors="replace").read() for p in srcs)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build",
                                                          "psxrecomp-game")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    with tempfile.TemporaryDirectory() as tmp:
        src = generate(args.recompiler, tmp)

    temp = "psx_ldd_%08X" % LOAD          # the deferred load sits at LOAD
    failures = []

    # The pair must actually be modeled, otherwise this test proves nothing.
    if "MIPS-I load-delay pair" not in src:
        failures.append("no load-delay pair block emitted; the dependent "
                        "lw/lwr pair was not modeled at all")

    # The LWL/LWR merge operand must be the deferred temp, not the raw GPR.
    merge = re.search(r"psx_lwr\(cpu,\s*[^,]+,\s*([A-Za-z0-9_\[\]>c\-\.]+)\s*,", src)
    if not merge:
        failures.append("no psx_lwr call found in the emitted body")
    elif merge.group(1) != temp:
        failures.append("psx_lwr merge operand is %r, expected the forwarded "
                        "temp %r (a stale merge base drops the pending load)"
                        % (merge.group(1), temp))

    # ...and the deferred writeback must NOT be discarded: the merge consumed it.
    if re.search(r"\(void\)%s\s*;" % re.escape(temp), src):
        failures.append("deferred load %s is discarded; the LWL/LWR merge "
                        "already consumed it, so the load would vanish" % temp)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        raise SystemExit(1)

    print("LWL/LWR load-delay forwarding test passed (merge base = %s)" % temp)


if __name__ == "__main__":
    main()
