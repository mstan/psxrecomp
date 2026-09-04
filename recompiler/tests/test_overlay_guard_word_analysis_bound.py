#!/usr/bin/env python3
"""The overlay delay-slot guard word is READ-legal but ANALYSIS-illegal.

An overlay capture deliberately carries one coherent guard instruction past
the end of a dirty-page run (runtime/src/overlay_capture.c write_json_window,
`size += 4u`), so that a MIPS branch sitting at the run's final word (...FFC)
has its architectural delay slot (...000) available. That makes the guard word
a legal DELAY-SLOT SOURCE and an illegal BLOCK LEADER: the word AFTER it does
not exist in the image, so a control transfer AT the guard word could never
emit its own mandatory delay slot.

The producer states the guard-byte count in the PS-EXE analysis-bound tag
(tools/compile_overlays.py make_psxexe -> recompiler/include/ps1_exe_parser.h
namespace exe_tag). The recompiler must then:

  A. TAGGED, guard word decodes as control flow, word before it does not.
     This is the exact shape that failed 17 of 67 Ape Escape (SCUS-94423)
     shards at overlay 0x80136000 (bead beads-eio.3.100): discovery walked
     into the guard word, block construction made it the last block's LEADER,
     the "also check last instruction itself" promotion made it that block's
     exit_instr, and the emitter correctly refused to emit a transfer whose
     delay slot it could not read. Generation must now SUCCEED, the guard word
     must not be a block leader, and control must leave the unit by publishing
     the guard-word PC for the runtime to dispatch.

  B. UNTAGGED, byte-for-byte identical payload. The producer says there is no
     guard word, so the last word IS ordinary code with a missing mandatory
     delay slot, and generation must still FAIL CLOSED with the delay-slot
     diagnostic. Emitting a control transfer without its architectural delay
     slot silently corrupts guest state; that throw is correct and must
     survive any fix to (A). A and B differ ONLY in the header tag, so this
     pair proves the tag is what changed behaviour and that nothing was
     weakened.

  C. TAGGED, with a real branch at the last analysable word. The guard word
     must still be readable AS a delay slot, must be emitted, and must be
     covered by the range manifest so its bytes join the candidate CRC and the
     page-generation watch.

  D. No range in the manifest may ever claim a byte past the image end. The
     +4 extension in generate_ranges_manifest fires for any block whose exit
     instruction IS its last word; a branch-likely opcode (0x14-0x17, RESERVED
     on the R3000A) as the image's very last word reaches that extension with
     generation SUCCEEDING, because translate_basic_block short-circuits those
     into an inline RI raise before any delay slot is read. Unclamped, the
     manifest then hashes 4 bytes the shard never saw and the cache validates
     against garbage.

Usage:  test_overlay_guard_word_analysis_bound.py [--recompiler <exe>]
Exit 0 = PASS.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

LOAD = 0x80010000
PAGE_BYTES = 4096

# Analysis-bound tag; must stay in lockstep with ps1_exe_parser.h exe_tag and
# with tools/compile_overlays.py.
GUARD_TAG_MAGIC_OFFSET = 0x7E0
GUARD_TAG_COUNT_OFFSET = 0x7E8
GUARD_TAG_MAGIC = b"PSXRGRD1"

NOP = 0x00000000
ADDIU_V0_0x2000 = 0x24022000   # addiu $v0, $zero, 0x2000 — NOT control flow
BEQ_V1_V0_FWD = 0x1062005A     # beq $v1, $v0, +0x5A     — IS control flow
BEQ_ZERO_FWD = 0x10000002      # beq $zero, $zero, +2    — IS control flow
BEQL_RESERVED = 0x50800008     # beql — RESERVED on the R3000A (opcode 0x14)


def words(seq):
    return b"".join(struct.pack("<I", w) for w in seq)


def make_psxexe(data: bytes, guard_bytes: int) -> bytes:
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)          # initial PC
    struct.pack_into("<I", header, 0x18, LOAD)          # load address
    struct.pack_into("<I", header, 0x1C, len(data))     # text size
    if guard_bytes:
        header[GUARD_TAG_MAGIC_OFFSET:
               GUARD_TAG_MAGIC_OFFSET + len(GUARD_TAG_MAGIC)] = GUARD_TAG_MAGIC
        struct.pack_into("<I", header, GUARD_TAG_COUNT_OFFSET, guard_bytes)
    return bytes(header) + data


def run_codegen(recompiler, root, case, data, guard_bytes):
    case_dir = os.path.join(root, case)
    out_dir = os.path.join(case_dir, "out")
    os.makedirs(out_dir)
    psx = os.path.join(case_dir, "region.psx")
    seeds = os.path.join(case_dir, "seeds.txt")
    with open(psx, "wb") as f:
        f.write(make_psxexe(data, guard_bytes))
    with open(seeds, "w", encoding="ascii") as f:
        f.write(f"dispatch_root 0x{LOAD:08X}\n")
    proc = subprocess.run(
        [recompiler, psx, "--seeds", seeds, "--out-dir", out_dir, "--overlay"],
        capture_output=True, text=True)
    return proc, out_dir


def read_generated(out_dir):
    source = ""
    manifest = ""
    for name in sorted(os.listdir(out_dir)):
        path = os.path.join(out_dir, name)
        if name.endswith(".c"):
            with open(path, encoding="utf-8") as f:
                source += f.read()
        elif name.endswith(".ranges"):
            with open(path, encoding="utf-8") as f:
                manifest += f.read()
    return source, manifest


def manifest_ranges(manifest):
    out = []
    for line in manifest.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == "R":
            lo = int(fields[1], 16)
            out.append((lo, lo + int(fields[2], 16)))
    return out


def guard_shape_payload():
    """2 pages whose last real word is NOT control flow, plus a guard word
    that IS — the measured Ape 0x80136000 shape (bead beads-eio.3.100)."""
    body = [NOP] * ((0x2000 // 4) - 1) + [ADDIU_V0_0x2000]
    return words(body + [BEQ_V1_V0_FWD])


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_recompiler = os.path.normpath(
        os.path.join(here, "..", "build", "psxrecomp-game.exe"))
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=default_recompiler)
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    failures = []
    payload = guard_shape_payload()
    image_end = LOAD + len(payload)          # 0x80012004
    guard_pc = image_end - 4                 # 0x80012000
    last_analysable = guard_pc - 4           # 0x80011FFC

    with tempfile.TemporaryDirectory() as root:
        # ---- A: tagged guard word must not be analysed as code -------------
        proc, out_dir = run_codegen(args.recompiler, root, "tagged_guard",
                                    payload, 4)
        combined = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            failures.append(
                "tagged guard word still fails generation: " +
                next((l for l in combined.splitlines() if "what()" in l),
                     combined[-400:]))
        else:
            source, manifest = read_generated(out_dir)
            if f"0x{last_analysable:08X}" not in source:
                failures.append(
                    "tagged guard image did not emit its last analysable "
                    f"word 0x{last_analysable:08X}")
            # The guard word must never be emitted as an executed instruction.
            # It may only appear as a published PC handed back to the runtime.
            for line in source.splitlines():
                if f"0x{guard_pc:08X}" not in line:
                    continue
                if "cpu->pc =" in line or "psx_check_interrupts_at" in line:
                    continue
                failures.append(
                    "guard word was analysed as code, not just published as a "
                    f"PC: {line.strip()}")
            if f"cpu->pc = 0x{guard_pc:08X}u" not in source:
                failures.append(
                    "tagged guard image does not publish the guard-word PC "
                    "for runtime dispatch at the unit edge")
            for lo, hi in manifest_ranges(manifest):
                if lo <= guard_pc < hi:
                    failures.append(
                        f"range [0x{lo:08X},0x{hi:08X}) claims the guard word "
                        "even though no emitted instruction uses it")

        # ---- B: identical bytes, no tag -> the throw must still fire -------
        proc_b, _ = run_codegen(args.recompiler, root, "untagged_same_bytes",
                                payload, 0)
        combined_b = (proc_b.stdout or "") + (proc_b.stderr or "")
        if proc_b.returncode == 0:
            failures.append(
                "untagged image with a missing mandatory delay slot generated "
                "a shard instead of failing closed")
        elif "mandatory delay slot" not in combined_b:
            failures.append(
                "untagged image failed without the delay-slot diagnostic: " +
                combined_b[-400:])
        elif f"0x{guard_pc:08X}" not in combined_b:
            failures.append(
                "delay-slot diagnostic did not name the offending transfer PC "
                f"0x{guard_pc:08X}: " + combined_b[-400:])

        # ---- B2: a malformed declaration must fail closed, not open -------
        # 6 is not instruction-aligned, so decode_analysis_guard_bytes must
        # reject it and fall back to "no guard" -- i.e. behave exactly like B.
        proc_b2, _ = run_codegen(args.recompiler, root, "malformed_tag",
                                 payload, 6)
        combined_b2 = (proc_b2.stdout or "") + (proc_b2.stderr or "")
        if proc_b2.returncode == 0:
            failures.append(
                "a misaligned guard-byte declaration widened analysis instead "
                "of being rejected")
        elif "mandatory delay slot" not in combined_b2:
            failures.append(
                "misaligned guard-byte declaration failed for the wrong "
                "reason: " + combined_b2[-400:])

        # ---- C: the guard word must still work AS a delay slot ------------
        branch_body = [NOP] * ((0x2000 // 4) - 1) + [BEQ_ZERO_FWD]
        branch_payload = words(branch_body + [ADDIU_V0_0x2000])
        proc_c, out_c = run_codegen(args.recompiler, root, "guard_is_slot",
                                    branch_payload, 4)
        if proc_c.returncode != 0:
            failures.append(
                "branch at the last analysable word could not use the guard "
                "word as its delay slot: " +
                ((proc_c.stderr or "") + (proc_c.stdout or ""))[-400:])
        else:
            source_c, manifest_c = read_generated(out_c)
            if f"0x{ADDIU_V0_0x2000:08X}" not in source_c:
                failures.append(
                    "guard word was not emitted as the branch's delay slot")
            covered = any(lo <= guard_pc and guard_pc + 4 <= hi
                          for lo, hi in manifest_ranges(manifest_c))
            if not covered:
                failures.append(
                    "range manifest does not hash the guard word that is "
                    "compiled in as a delay slot")

        # ---- D: no range may exceed the captured image end -----------------
        # beql as the very last word: RESERVED on the R3000A, so
        # translate_basic_block emits an inline RI raise and never reads a
        # delay slot -- generation succeeds and the +4 range extension fires.
        beql_payload = words([NOP] * 6 + [BEQL_RESERVED])
        beql_end = LOAD + len(beql_payload)
        proc_d, out_d = run_codegen(args.recompiler, root, "beql_tail",
                                    beql_payload, 0)
        if proc_d.returncode != 0:
            failures.append(
                "reserved branch-likely tail aborted generation: " +
                ((proc_d.stderr or "") + (proc_d.stdout or ""))[-400:])
        else:
            _, manifest_d = read_generated(out_d)
            ranges_d = manifest_ranges(manifest_d)
            if not ranges_d:
                failures.append("reserved branch-likely tail emitted no ranges")
            for lo, hi in ranges_d:
                if hi > beql_end:
                    failures.append(
                        f"range [0x{lo:08X},0x{hi:08X}) claims "
                        f"{hi - beql_end} byte(s) past the image end "
                        f"0x{beql_end:08X}")

    if failures:
        for failure in failures:
            print("FAIL:", failure)
        return 1
    print("PASS: guard word is a delay-slot source and never a block leader; "
          "the mandatory-delay-slot throw still fires on an untagged image; "
          "no range manifest claims a byte past the captured image end")
    return 0


if __name__ == "__main__":
    sys.exit(main())
