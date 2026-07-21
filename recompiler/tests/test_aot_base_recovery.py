#!/usr/bin/env python3
"""Focused regression checks for generic AOT overlay base selection."""
import collections
import importlib.util
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "extract_generic", ROOT / "tools" / "aot_overlay_spike" /
    "extract_generic.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)


def main():
    true_base = 0x80106228
    nearby_false = 0x80106220

    # A near-tie without independent evidence fails closed. If a call-dense
    # sibling has already proved the exact true base, that one unique match is
    # enough to resolve the call-sparse file.
    votes = collections.Counter({nearby_false: 13, true_base: 12})
    assert MOD._select_base_vote(votes) is None
    assert MOD._select_base_vote(votes, {true_base}) == (true_base, 12)
    assert MOD._select_base_vote(votes, {true_base, nearby_false}) is None

    # Only JAL is callable-boundary evidence. A nearby unconditional J branch
    # must not participate in base recovery.
    data = bytearray(12)
    struct.pack_into("<I", data, 0, 0x08000000 | ((0x80101234 >> 2) & 0x03FFFFFF))
    struct.pack_into("<I", data, 4, 0x0C000000 | ((0x80105678 >> 2) & 0x03FFFFFF))
    assert MOD.jal_targets(data) == {0x80105678}

    # A JAL target becomes a conservative shard root only when the target is
    # local and has independent callable-boundary evidence.
    rooted = bytearray(0x80)
    struct.pack_into('<I', rooted, 0, 0x0C000000 |
                     (((0x80100000 + 0x40) >> 2) & 0x03FFFFFF))
    struct.pack_into('<I', rooted, 0x40, 0x27BDFFF0)
    assert MOD.direct_jal_roots(rooted, 0x80100000) == [0x80100040]
    struct.pack_into('<I', rooted, 0x40, 0x24020001)
    assert MOD.direct_jal_roots(rooted, 0x80100000) == []

    # A return boundary plus a JAL-shaped source is still not enough when the
    # alleged target begins with a dense run of local pointers. Those words are
    # data even though the first one has a syntactically valid MIPS decode.
    pointer_root = bytearray(0x100)
    struct.pack_into('<I', pointer_root, 0, 0x0C000000 |
                     (((0x80100000 + 0x40) >> 2) & 0x03FFFFFF))
    struct.pack_into('<I', pointer_root, 0x38, 0x03E00008)
    struct.pack_into('<I', pointer_root, 0x3C, 0x00000000)
    for index, target in enumerate((0x60, 0x70, 0x80)):
        struct.pack_into('<I', pointer_root, 0x40 + index * 4,
                         0x80100000 + target)
    assert MOD.direct_jal_roots(pointer_root, 0x80100000) == []

    bounded = bytearray(0x90)
    for off in (0x10, 0x40, 0x70):
        struct.pack_into('<I', bounded, off, 0x27BDFFF0)
    recipe = MOD.bounded_dispatch_fallback(
        bounded, 0x80100000, [0x80100024, 0x80100050])
    assert recipe['function_entry_pcs'] == ['0x80100010', '0x80100040']
    assert recipe['dispatch_entry_pcs'] == [
        '0x80100010', '0x80100024', '0x80100040', '0x80100050']
    assert set(recipe['static_dispatch_entry_pcs']) <= set(
        recipe['dispatch_entry_pcs'])
    assert recipe['producer_ranges'] == [
        {'start': '0x80100010', 'end': '0x80100040'},
        {'start': '0x80100040', 'end': '0x80100070'},
    ]

    wrapped = MOD.make_psx_exe(b'\x11\x22\x33\x44', 0x80102000,
                               0x80102040)
    assert wrapped[:8] == b'PS-X EXE'
    assert struct.unpack_from('<III', wrapped, 0x10) == (
        0x80102040, 0, 0x80102000)
    assert struct.unpack_from('<I', wrapped, 0x1C)[0] == 4
    assert wrapped[0x800:] == b'\x11\x22\x33\x44'

    # Psy-Q may schedule a global load before allocating the stack frame.  The
    # exact function starts immediately after the previous return, not at the
    # otherwise recognizable addiu-sp instruction eight bytes later.
    base = 0x80100000
    data = bytearray(0x40)
    struct.pack_into('<I', data, 0x00, 0x03E00008)  # jr ra
    struct.pack_into('<I', data, 0x04, 0x00000000)  # delay slot
    struct.pack_into('<I', data, 0x08, 0x3C03800A)  # lui v1,0x800a
    struct.pack_into('<I', data, 0x0C, 0x9463BCC8)  # lhu v1,-0x4338(v1)
    struct.pack_into('<I', data, 0x10, 0x27BDFFE8)  # addiu sp,sp,-0x18
    assert MOD.prologues(data, base) == [base + 0x08]
    # A load through a different register is not sufficient evidence to move
    # the entry; fail closed at the ordinary stack-frame prologue.
    struct.pack_into('<I', data, 0x0C, 0x9443BCC8)  # lhu v1,-0x4338(v0)
    assert MOD.prologues(data, base) == [base + 0x10]

    # Normal-mode discovery must not turn a PS-X body's provenance-free leading
    # pointer table into a function. Keep the declared entry and the additive
    # decoder-derived interior set; those entries have already passed normal
    # mode's return/prologue/call/control-flow discovery and classification.
    data = bytearray(0x100)
    for n in range(4):
        struct.pack_into('<I', data, n * 4, base + 0x40 + n * 4)
    struct.pack_into('<I', data, 0x40, 0x27BDFFF0)
    struct.pack_into('<I', data, 0x44, 0x0C000000 |
                     (((base + 0x80) >> 2) & 0x03FFFFFF))
    struct.pack_into('<I', data, 0x80, 0x24020001)
    assert MOD.filter_full_discovery_seeds(
        data, base, [base, base + 0x40, base + 0x80], base + 0x40) == [
            base + 0x40, base + 0x80]
    assert MOD.filter_full_discovery_seeds(
        data, base, [base, base + 0x40, base + 0x80], base) == [
            base, base + 0x40, base + 0x80]

    entries, aliases = MOD.parse_full_discovery_ranges([
        'F 80100040\n', 'R 80100040 20\n',
        'F 80100048\n', 'R 80100040 20\n'])
    assert entries == [base + 0x40, base + 0x48]
    assert aliases == [(base + 0x48, base + 0x40, base + 0x60)]

    # Dense function-pointer tables may recover framed or frameless functions,
    # but only when every target has independent callable-boundary evidence.
    data = bytearray(0x100)
    # Three adjacent leaves, each immediately after a previous return.
    for off in (0x18, 0x28, 0x38):
        struct.pack_into('<I', data, off-8, 0x03E00008)
        struct.pack_into('<I', data, off-4, 0x00000000)
        struct.pack_into('<I', data, off, 0x24020001)
    for i, off in enumerate((0x18, 0x28, 0x38)):
        struct.pack_into('<I', data, 0x80+i*4, base+off)
    assert MOD.pointer_table_targets(data, base) == {
        base+0x18, base+0x28, base+0x38}
    # An isolated pointer-shaped word is intentionally insufficient.
    struct.pack_into('<I', data, 0x80, 0)
    struct.pack_into('<I', data, 0x84, 0)
    assert MOD.pointer_table_targets(data, base) == set()

    # An unreferenced frameless leaf can still be proven play-free by exact
    # return-to-return boundaries plus a bounded valid CFG.
    leaf = bytearray(0x80)
    struct.pack_into('<I', leaf, 0x00, 0x03E00008)
    struct.pack_into('<I', leaf, 0x04, 0x24020000)  # arbitrary delay slot
    struct.pack_into('<I', leaf, 0x08, 0x24020001)
    struct.pack_into('<I', leaf, 0x0C, 0x03E00008)
    struct.pack_into('<I', leaf, 0x10, 0x00000000)
    assert base + 0x08 in MOD.frameless_leaf_entries(leaf, base)
    # The same positional evidence must not promote a dense pointer table.
    for index, off in enumerate((0x40, 0x50, 0x60)):
        struct.pack_into('<I', leaf, 0x08 + index * 4, base + off)
    assert base + 0x08 not in MOD.frameless_leaf_entries(leaf, base)

    # A strict aligned {id,size} archive can share an independently voted link
    # base across members.  Direct JAL targets are the roots; unrelated padding
    # and prologue-shaped words do not become code merely by being in a member.
    archive_base = 0x800E9060
    members = []
    target_offsets = (0x80, 0x94, 0xB0, 0xD8,
                      0x110, 0x154, 0x1A8, 0x210)
    for ident in range(1, 5):
        body = bytearray(0x240)
        for n, target_off in enumerate(target_offsets):
            target = archive_base + target_off
            struct.pack_into('<I', body, n * 4, 0x0C000000 |
                             ((target >> 2) & 0x03FFFFFF))
            struct.pack_into('<I', body, target_off, 0x27BDFFF0)
        members.append((ident, bytes(body)))
    archive = bytearray(0x800)
    pos = 0x800
    payload = bytearray()
    for n, (ident, body) in enumerate(members):
        struct.pack_into('<II', archive, n * 8, ident, len(body))
        payload.extend(body)
        payload.extend(b'\0' * ((-len(payload)) & 0x7FF))
    archive.extend(payload)
    split = MOD.split_indexed_archive(bytes(archive))
    assert split is not None and [x[0] for x in split] == [1, 2, 3, 4]
    recovered = MOD.recover_indexed_archive(
        bytes(archive), 0x80010000, 0x80200000)
    assert len(recovered) == 4
    assert {x['base'] for x in recovered} == {archive_base}
    assert all(len(x['direct_seeds']) == 8 for x in recovered)

    # Ape HED member 0186 shape: the switch host is not directly called, but a
    # return-adjacent framed prologue proves its real root. Its bounded table
    # labels and reachable JAL continuation are dispatch evidence only.
    ape_base = 0x80136000
    ape_members = []
    anchor_offsets = (0x80, 0x94, 0xB0, 0xD8,
                      0x110, 0x154, 0x1A8, 0x210)
    table_values = ([ape_base + 0x22E4, ape_base + 0x2354,
                     ape_base + 0x22F4, ape_base + 0x22B8] +
                    [ape_base + 0x2354] * 2 +
                    [ape_base + 0x2304, ape_base + 0x2354,
                     ape_base + 0x2314, ape_base + 0x2354,
                     ape_base + 0x2334, ape_base + 0x2354,
                     ape_base + 0x2344, ape_base + 0x2354] +
                    [ape_base + 0x22B8, ape_base + 0x2354,
                     ape_base + 0x2354, ape_base + 0x2324] +
                    [ape_base + 0x22B8] * 28)
    assert len(table_values) == 46
    for ident in range(1, 5):
        body = bytearray(0x5000)
        for index, target_off in enumerate(anchor_offsets):
            target = ape_base + target_off
            struct.pack_into('<I', body, index * 4, 0x0C000000 |
                             ((target >> 2) & 0x03FFFFFF))
            struct.pack_into('<I', body, target_off, 0x27BDFFF0)
        struct.pack_into('<I', body, 0x2294, 0x03E00008)  # prior jr ra
        struct.pack_into('<I', body, 0x2298, 0x27BD0010)  # delay slot
        switch_words = {
            0x229C: 0x27BDFFE0, 0x22A0: 0xAFB10014,
            0x22A4: 0x3C118014, 0x22A8: 0x3C028014,
            0x22AC: 0xAFB00010, 0x22B0: 0x2450ABD8,
            0x22B4: 0xAFBF0018, 0x22B8: 0x8623AF56,
            0x22BC: 0x00000000, 0x22C0: 0x2C62002E,
            0x22C4: 0x1040FFFF, 0x22C8: 0x00000000,
            0x22CC: 0x00031080, 0x22D0: 0x00501021,
            0x22D4: 0x8C420000, 0x22D8: 0x00000000,
            0x22DC: 0x00400008, 0x22E0: 0x00000000,
        }
        for offset, word in switch_words.items():
            struct.pack_into('<I', body, offset, word)
        for offset in (0x22E4, 0x22F4, 0x2304, 0x2314,
                       0x2324, 0x2334, 0x2344):
            struct.pack_into('<I', body, offset, 0x03E00008)
            struct.pack_into('<I', body, offset + 4, 0x00000000)
        struct.pack_into('<I', body, 0x2354, 0x0C004000)  # jal 0x80010000
        struct.pack_into('<I', body, 0x2358, 0x00000000)
        struct.pack_into('<I', body, 0x235C, 0x24040004)
        struct.pack_into('<I', body, 0x2360, 0x0C004000)
        struct.pack_into('<I', body, 0x2364, 0x00000000)
        struct.pack_into('<I', body, 0x2368, 0x0804E0AE)  # j 0x801382B8
        struct.pack_into('<I', body, 0x236C, 0x00000000)
        for index, target in enumerate(table_values):
            struct.pack_into('<I', body, 0x4BD8 + index * 4, target)
        ape_members.append((ident, ident * 0x5000, bytes(body)))
    ape_recovered = MOD.recover_consensus_members(
        ape_members, 0x80100000, 0x80200000)
    assert len(ape_recovered) == 4
    for member in ape_recovered:
        assert ape_base + 0x229C in member['return_framed_seeds']
        assert ape_base + 0x229C in member['seeds']
        assert ape_base + 0x22B8 in member['static_dispatch_pcs']
        assert ape_base + 0x2354 in member['static_dispatch_pcs']
        assert ape_base + 0x2368 in member['static_dispatch_pcs']
        proof = next(proof for proof in member['jump_table_proofs']
                     if proof['jr_pc'] == ape_base + 0x22DC)
        assert proof == {
            'host_entry': ape_base + 0x229C,
            'jr_pc': ape_base + 0x22DC,
            'table_base': ape_base + 0x4BD8,
            'count': 46,
            'targets': sorted(set(table_values)),
        }
        assert ape_base + 0x2368 in member['call_continuation_pcs']
        capture = MOD.rec(
            ape_base, member['data'], member['seeds'],
            dispatch_extra=member['static_dispatch_pcs'],
            producer_ranges=((ape_base, ape_base + len(member['data'])),),
            static_discovery=member['direct_seeds'],
            jump_table_proofs=member['jump_table_proofs'],
            call_continuations=member['call_continuation_pcs'])
        assert '0x8013829C' in capture['function_entry_pcs']
        assert '0x801382B8' not in capture['function_entry_pcs']
        assert '0x80138354' not in capture['function_entry_pcs']
        assert '0x80138368' not in capture['function_entry_pcs']
        assert '0x801382B8' in capture['dispatch_entry_pcs']
        assert '0x80138354' in capture['dispatch_entry_pcs']
        assert '0x80138368' in capture['dispatch_entry_pcs']
        assert '0x801382B8' in capture['static_dispatch_entry_pcs']
        assert '0x80138354' in capture['static_dispatch_entry_pcs']
        assert '0x80138368' in capture['static_dispatch_entry_pcs']
        assert '0x8013829C' in capture['static_dispatch_entry_pcs']
        assert capture['static_jump_table_proofs'][0]['host_entry'] == (
            '0x8013829C')
        assert capture['static_jump_table_proofs'][0]['jr_pc'] == '0x801382DC'
        assert capture['static_jump_table_proofs'][0]['table_base'] == (
            '0x8013ABD8')
        assert capture['static_jump_table_proofs'][0]['count'] == 46
        assert '0x80138368' in capture['static_call_continuation_pcs']
        MOD.add_consensus_fallback(capture, member)
        assert capture['optional_enrichment_fallback_entry_pcs'] == [
            f'0x{addr:08X}' for addr in member['direct_seeds']]

    # Companion HED tables may address a logical sector namespace spanning DAT
    # then BNS. Runs are discovered independently across zero-filled table gaps.
    hed = bytearray(0x800)
    for n in range(4):
        struct.pack_into('<I', hed, n * 4, (1 << 20) | n)
        struct.pack_into('<I', hed, 0x100 + n * 4, (1 << 20) | (4 + n))
    payload_a = b''.join(bytes([n + 1]) * 0x800 for n in range(4))
    payload_b = b''.join(bytes([n + 5]) * 0x800 for n in range(4))

    class FakeDisc:
        blobs = {1: bytes(hed), 2: payload_a, 3: payload_b}

        def read_file_bytes(self, lba, size):
            return self.blobs[lba][:size]

    groups = MOD.hed_companion_members(FakeDisc(), [
        ('ARC.HED', 1, len(hed)), ('ARC.DAT', 2, len(payload_a)),
        ('ARC.BNS', 3, len(payload_b)),
    ])
    assert len(groups) == 1 and len(groups[0]['members']) == 8
    assert [x[1] for x in groups[0]['members']] == [n * 0x800
                                                    for n in range(8)]

    # Exact-hash BIOS resident recipes synthesize only their declared code and
    # data words. Entries must point into code fragments; a different BIOS is a
    # safe empty result rather than a speculative shard.
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        bios = td / 'bios.bin'
        bios.write_bytes(b'test bios')
        manifest = td / 'resident.json'
        manifest.write_text(json.dumps({
            'schema': 'psxrecomp bios resident code v1',
            'images': [{
                'name': 'test helper',
                'bios_sha256': hashlib.sha256(b'test bios').hexdigest(),
                'region_start': '0x8000DF80',
                'region_size': '0x80',
                'code_fragments': [{
                    'address': '0x8000DF80',
                    'words': ['0x03E00008', '0x00000000'],
                }],
                'data_words': [{
                    'address': '0x8000DFFC', 'word': '0x12345678',
                }],
                'dispatch_entry_pcs': ['0x8000DF80'],
            }],
        }), encoding='utf-8')
        resident = MOD.bios_resident_records(str(bios), str(manifest))
        assert len(resident) == 1
        MOD.enforce_bios_resident_policy(True, False, resident)
        capture = resident[0]
        decoded = __import__('base64').b64decode(capture['bytes_b64'])
        assert struct.unpack_from('<I', decoded, 0)[0] == 0x03E00008
        assert struct.unpack_from('<I', decoded, 0x7C)[0] == 0x12345678
        assert capture['producer_ranges'] == [{
            'start': '0x8000DF80', 'end': '0x8000DF88'}]
        bios.write_bytes(b'different bios')
        no_match = MOD.bios_resident_records(str(bios), str(manifest))
        assert no_match == []
        try:
            MOD.enforce_bios_resident_policy(True, False, no_match)
            assert False, 'required missing BIOS recipe was accepted'
        except ValueError as exc:
            assert 'no recipe matching' in str(exc)
        try:
            MOD.enforce_bios_resident_policy(True, True, resident)
            assert False, 'required+disabled BIOS policy was accepted'
        except ValueError as exc:
            assert 'conflicts' in str(exc)

        # Exercise the actual CLI contract: explicit BIOS resolution, required
        # success, fail-closed mismatch, output preservation, and flag errors
        # that are reported before any manifest/BIOS preflight.
        script = str(ROOT / 'tools' / 'aot_overlay_spike' /
                     'extract_generic.py')
        out = td / 'captures.json'
        bios.write_bytes(b'test bios')
        result = subprocess.run([
            sys.executable, script, '--out', str(out), '--bios', str(bios),
            '--bios-resident-manifest', str(manifest),
            '--only-bios-resident', '--require-bios-resident',
        ], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        assert len(json.loads(out.read_text())) == 1

        out.write_text('preserve-me', encoding='utf-8')
        bios.write_bytes(b'different bios')
        result = subprocess.run([
            sys.executable, script, '--out', str(out), '--bios', str(bios),
            '--bios-resident-manifest', str(manifest),
            '--only-bios-resident', '--require-bios-resident',
        ], capture_output=True, text=True)
        assert result.returncode == 2
        assert 'no recipe matching' in result.stderr
        assert out.read_text() == 'preserve-me'

        result = subprocess.run([
            sys.executable, script, '--out', str(out),
            '--bios-resident-manifest', str(td / 'must-not-be-read.json'),
            '--require-bios-resident', '--no-bios-resident',
        ], capture_output=True, text=True)
        assert result.returncode == 2
        assert '--require-bios-resident conflicts' in result.stderr
        assert out.read_text() == 'preserve-me'

        result = subprocess.run([
            sys.executable, script, '--out', str(out),
            '--only-bios-resident', '--no-bios-resident',
        ], capture_output=True, text=True)
        assert result.returncode == 2
        assert '--only-bios-resident conflicts' in result.stderr
        assert out.read_text() == 'preserve-me'

    print("ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
