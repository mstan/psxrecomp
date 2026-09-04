#!/usr/bin/env python3
"""Recompiler codegen regression test: KUSEG-addressed games must dispatch.

PS1 segments alias the same physical RAM. Most PS-X EXE headers carry KSEG
addresses (0x8001xxxx), but a header may legally carry KUSEG ones instead
(0x0001xxxx) -- Alien Resurrection (SLUS-00633) does, and it then executes with
a KUSEG PC.

The recompiler normalizes the dispatch table to KSEG keys. When the lookup
compared raw values, a KUSEG PC could never match: 0x0001xxxx is always below
0x8001xxxx, so the binary search collapsed to the start and returned no entry.
Every dispatch fell through to the interpreter -- silently, because the
text-image guard was never even reached (it reported zero blocks and zero
mismatches while the whole game ran interpreted).

This test synthesizes a KUSEG-addressed PS-EXE and asserts the emitted lookup
compares 29-bit physical addresses, and that the table is ordered by that same
key so the binary-search invariant holds.

Usage:  python test_kuseg_dispatch_lookup.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, os, re, struct, subprocess, sys, tempfile

LOAD = 0x00010000          # KUSEG: no KSEG bit, the case under test
PHYS_MASK = 0x1FFFFFFF


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)   # stack_base, as real headers carry
    return bytes(h) + data


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def build_exe():
    # func A @ LOAD: addiu sp,-8 ; jal B ; nop ; addiu sp,8 ; jr ra ; nop
    # func B @ LOAD+0x20: jr ra ; nop
    a = [0x27BDFFF8, jal(LOAD + 0x20), 0x00000000, 0x27BD0008, 0x03E00008, 0x00000000]
    body = bytearray(w(a))
    body += b"\x00" * (0x20 - len(body))
    body += w([0x03E00008, 0x00000000])
    return make_psxexe(LOAD, LOAD, bytes(body))


def gen_dispatch(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    with open(psx, "wb") as f:
        f.write(build_exe())
    with open(seeds, "w") as f:
        f.write("0x%08X\n0x%08X\n" % (LOAD, LOAD + 0x20))
    r = subprocess.run([recompiler, psx, "--seeds", seeds, "--out-dir", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
    disp = [f for f in os.listdir(out) if f.endswith("_dispatch.c")]
    if not disp:
        raise SystemExit("no _dispatch.c emitted in " + out)
    with open(os.path.join(out, disp[0])) as f:
        return f.read()


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
        src = gen_dispatch(args.recompiler, tmp)

    m = re.search(r"static const PsxGameDispatchEntry\* psx_game_find_entry"
                  r"\(uint32_t addr\) \{(.*?)\n\}", src, re.DOTALL)
    if not m:
        raise SystemExit("psx_game_find_entry not found in emitted dispatch")
    body = m.group(1)

    # The incoming PC must be reduced to a physical address before comparing.
    if "0x1FFFFFFFu" not in body:
        raise SystemExit(
            "psx_game_find_entry does not mask to a physical address; a KUSEG "
            "guest PC can never match KSEG-normalized table keys")
    # ... and so must the stored key, otherwise the comparison is still mixed.
    if not re.search(r"k_psx_game_dispatch\[mid\]\.addr\s*&\s*0x1FFFFFFFu", body):
        raise SystemExit(
            "psx_game_find_entry compares against an unmasked table key")

    # The binary search requires the table to be ordered by the same key it
    # compares. Parse the emitted entries and assert monotonic physical order.
    tbl = re.search(r"k_psx_game_dispatch\[\] = \{(.*?)\n\};", src, re.DOTALL)
    if not tbl:
        raise SystemExit("dispatch table not found in emitted dispatch")
    keys = [int(x, 16) & PHYS_MASK
            for x in re.findall(r"\{0x([0-9A-Fa-f]{8})u,", tbl.group(1))]
    if len(keys) < 2:
        raise SystemExit("expected at least two dispatch entries, got %d" % len(keys))
    if keys != sorted(keys):
        raise SystemExit("dispatch table is not sorted by physical address; "
                         "the masked binary search would miss entries")

    print("KUSEG dispatch lookup test passed (%d entries, physical-keyed)" % len(keys))


if __name__ == "__main__":
    main()
