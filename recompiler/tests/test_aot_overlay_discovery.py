#!/usr/bin/env python3
"""Focused regression checks for play-free overlay call-target discovery."""
import argparse
import importlib.util
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "compile_overlays", ROOT / "tools" / "compile_overlays.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)
EXTRACT_SPEC = importlib.util.spec_from_file_location(
    "extract_generic", ROOT / "tools" / "aot_overlay_spike" /
    "extract_generic.py")
EXTRACT = importlib.util.module_from_spec(EXTRACT_SPEC)
assert EXTRACT_SPEC.loader is not None
EXTRACT_SPEC.loader.exec_module(EXTRACT)

LOAD = 0x80010000


def put(data, offset, word):
    struct.pack_into("<I", data, offset, word)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def make_psxexe(entry, data):
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    put(header, 0x10, entry)
    put(header, 0x18, LOAD)
    put(header, 0x1C, len(data))
    return bytes(header) + data


def make_bounded_switch():
    """Ape-shaped cross-register table base and post-JR case bodies."""
    data = bytearray(0x300)
    entry = LOAD + 0x30
    table = LOAD + 0x200
    cases = (LOAD + 0x68, LOAD + 0x88, LOAD + 0x68, LOAD + 0x78)
    put(data, 0x30, 0x27BDFFE0)  # addiu sp,sp,-0x20
    put(data, 0x34, 0xAFBF0018)  # sw ra,0x18(sp)
    put(data, 0x38, 0x3C028001)  # lui v0,0x8001
    put(data, 0x3C, 0x24500200)  # addiu s0,v0,0x200 (register rename)
    put(data, 0x40, 0x24030000)  # addiu v1,zero,0
    put(data, 0x44, 0x2C620004)  # sltiu v0,v1,4
    put(data, 0x48, 0x1040FFFF)  # beq v0,zero,0x48 (reject loop)
    put(data, 0x4C, 0x00000000)
    put(data, 0x50, 0x00031080)  # sll v0,v1,2
    put(data, 0x54, 0x00501021)  # addu v0,v0,s0
    put(data, 0x58, 0x8C420000)  # lw v0,0(v0)
    put(data, 0x5C, 0x00000000)
    put(data, 0x60, 0x00400008)  # jr v0
    put(data, 0x64, 0x00000000)
    put(data, 0x68, 0x03E00008)
    put(data, 0x6C, 0x00000000)
    put(data, 0x78, 0x03E00008)
    put(data, 0x7C, 0x00000000)
    put(data, 0x88, jal(0x80008000))  # external call
    put(data, 0x8C, 0x00000000)
    put(data, 0x90, 0x03E00008)       # call continuation
    put(data, 0x94, 0x00000000)
    for index, target in enumerate(cases):
        put(data, 0x200 + index * 4, target)
    return data, entry, table, set(cases), LOAD + 0x90


def check_bounded_jump_table_discovery():
    data, entry, table, cases, continuation = make_bounded_switch()
    found = MOD._find_jump_table_targets(
        bytes(data), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2)
    assert found == cases
    zero_nops = bytearray(data)
    put(zero_nops, 0x5C, 0x00400008)
    assert MOD._find_jump_table_targets(
        bytes(zero_nops), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x5C, 2) == cases
    two_nops = bytearray(data)
    put(two_nops, 0x60, 0x00000000)
    put(two_nops, 0x64, 0x00400008)
    assert MOD._find_jump_table_targets(
        bytes(two_nops), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x64, 2) == set()
    three_nops = bytearray(two_nops)
    put(three_nops, 0x64, 0x00000000)
    put(three_nops, 0x68, 0x00400008)
    assert MOD._find_jump_table_targets(
        bytes(three_nops), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x68, 2) == set()
    walk = MOD._walk_overlay_function(
        bytes(data), LOAD, len(data), entry, LOAD + 0x100,
        ((LOAD, LOAD + len(data)),), False)
    assert walk['jump_table_targets'] == cases
    assert cases <= walk['visited']
    assert continuation in walk['call_continuations']
    assert continuation in walk['visited']

    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{entry:08X}'],
        'dispatch_entry_pcs': [
            f'0x{addr:08X}' for addr in sorted(cases | {continuation})],
        'producer_ranges': [{
            'start': f'0x{LOAD:08X}',
            'end': f'0x{LOAD + len(data):08X}',
        }],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert entry in audit['static_exact_fragment_demands']
    for addr in cases | {continuation}:
        assert audit['included_reasons'][addr] == 'DISPATCH_INTERIOR'
        assert addr not in audit['static_exact_fragment_demands']
        assert f'interior 0x{addr:08X}' in seeds
        assert f'call_root 0x{addr:08X}' not in seeds

    # Every relationship is mandatory; opcode proximity or repeated pointers
    # alone cannot manufacture a switch.
    mismatched_index = bytearray(data)
    put(mismatched_index, 0x44, 0x2C820004)  # sltiu v0,a0,4
    assert MOD._find_jump_table_targets(
        bytes(mismatched_index), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    clobbered_base = bytearray(data)
    put(clobbered_base, 0x40, 0x26100004)  # addiu s0,s0,4
    assert MOD._find_jump_table_targets(
        bytes(clobbered_base), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_jr = bytearray(data)
    put(noncanonical_jr, 0x60, 0x00410008)  # reserved rt field is nonzero
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_jr), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    zero_lw_base = bytearray(data)
    put(zero_lw_base, 0x58, 0x8C020000)  # lw v0,0(zero)
    assert MOD._find_jump_table_targets(
        bytes(zero_lw_base), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    zero_addu_dest = bytearray(data)
    put(zero_addu_dest, 0x54, 0x00500021)  # addu zero,v0,s0
    assert MOD._find_jump_table_targets(
        bytes(zero_addu_dest), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_addu = bytearray(data)
    put(noncanonical_addu, 0x54, 0x00501061)  # nonzero reserved shamt
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_addu), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_sll = bytearray(data)
    put(noncanonical_sll, 0x50, 0x00231080)  # SLL's reserved rs is nonzero
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_sll), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    noncanonical_lui = bytearray(data)
    put(noncanonical_lui, 0x38, 0x3C228001)  # LUI's reserved rs is nonzero
    assert MOD._find_jump_table_targets(
        bytes(noncanonical_lui), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    # A validated case in producer A cannot use producer B as a trampoline
    # back into the protected suffix. Otherwise B's bytes would manufacture a
    # false domination proof for A's table-base and bounds definitions.
    producer_escape = bytearray(data)
    producer_b = LOAD + 0x260
    put(producer_escape, 0x68,
        0x08000000 | ((producer_b >> 2) & 0x03FFFFFF))
    put(producer_escape, 0x6C, 0x00000000)
    put(producer_escape, 0x260,
        0x08000000 | (((LOAD + 0x50) >> 2) & 0x03FFFFFF))
    put(producer_escape, 0x264, 0x00000000)
    assert MOD._find_jump_table_targets(
        bytes(producer_escape), LOAD, len(data), entry, LOAD + 0x300,
        LOAD + 0x60, 2, (LOAD, LOAD + 0x240)) == set()

    ori_base = bytearray(data)
    put(ori_base, 0x3C, 0x34500200)  # ori s0,v0,0x200 is outside grammar
    assert MOD._find_jump_table_targets(
        bytes(ori_base), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    skipped_constant = bytearray(data)
    put(skipped_constant, 0x34, 0x08000000 |
        (((LOAD + 0x40) >> 2) & 0x03FFFFFF))
    assert MOD._find_jump_table_targets(
        bytes(skipped_constant), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    later_inbound = bytearray(data)
    put(later_inbound, 0xA0, 0x08000000 |
        (((LOAD + 0x50) >> 2) & 0x03FFFFFF))
    assert MOD._find_jump_table_targets(
        bytes(later_inbound), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()

    foreign_target = bytearray(data)
    put(foreign_target, table - LOAD, LOAD + 0x180)
    assert MOD._find_jump_table_targets(
        bytes(foreign_target), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2) == set()
    assert MOD._find_jump_table_targets(
        bytes(data), LOAD, len(data), entry, LOAD + 0x100,
        LOAD + 0x60, 2, (LOAD, LOAD + 0x180)) == set()


def check_composite_call_boundaries():
    data = bytearray(0x100)
    cross_target = LOAD + 0x50
    put(data, 0x00, jal(cross_target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x50, 0x24020001)  # valid MIPS, but no callable boundary
    ranges = ((LOAD, LOAD + 0x40), (LOAD + 0x40, LOAD + 0x100))

    # A direct JAL remains sufficient for frameless leaves inside one producer,
    # but not when an adjacent-producer variant merely puts pointer/data-shaped
    # bytes at the encoded address.
    unscoped = MOD._walk_overlay_function(data, LOAD, len(data), LOAD, LOAD + 0x100)
    assert cross_target in unscoped['direct_jals']
    scoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges)
    assert cross_target not in scoped['direct_jals']
    assert cross_target in scoped['rejected_cross_producer_calls']

    # A bounded frameless body with a real return is independently code-like
    # and remains discoverable across the producer boundary.
    put(data, 0x54, 0x03E00008)
    put(data, 0x58, 0x00000000)
    scoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges)
    assert cross_target in scoped['direct_jals']

    # Even within one nominal producer, a dense run of local pointers is data,
    # not a frameless callable merely because a reachable word looks like JAL.
    for index, offset in enumerate((0x70, 0x80, 0x90)):
        put(data, 0x50 + index * 4, LOAD + offset)
    unscoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100)
    assert cross_target not in unscoped['direct_jals']
    assert cross_target in unscoped['rejected_cross_producer_calls']

    strict = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges,
        allow_cross_producer_calls=False)
    assert cross_target not in strict['direct_jals']
    assert cross_target in strict['rejected_cross_producer_calls']

    # Independent target-local boundary evidence makes a real cross-producer
    # export safe to retain.
    put(data, 0x50, 0x27BDFFE0)
    scoped = MOD._walk_overlay_function(
        data, LOAD, len(data), LOAD, LOAD + 0x100, ranges)
    assert cross_target in scoped['direct_jals']


def check_static_discovery_provenance():
    data = bytearray(0x80)
    entry = LOAD + 0x20
    put(data, 0x20, 0x30620008)  # andi v0,v1,8 -- frameless body
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{entry:08X}'],
        'static_discovery_entry_pcs': [f'0x{entry:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert f'call_root 0x{entry:08X}' in seeds
    assert audit['included_reasons'][entry] == 'STATIC_DISCOVERY_ROOT'


def check_static_dispatch_provenance():
    data = bytearray(0x80)
    runtime_entry = LOAD + 0x20
    static_entry = LOAD + 0x40
    interior = LOAD + 0x60
    put(data, 0x20, 0x27BDFFF0)
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x27BD0010)
    put(data, 0x40, 0x27BDFFF0)
    put(data, 0x44, 0x10000006)  # branch to interior: block, not callable
    put(data, 0x48, 0)
    put(data, 0x4C, 0x03E00008)
    put(data, 0x50, 0x27BD0010)
    put(data, 0x60, 0x24420001)
    put(data, 0x64, 0x03E00008)
    put(data, 0x68, 0x27BD0010)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'dispatch_entry_pcs': [
            f'0x{runtime_entry:08X}', f'0x{static_entry:08X}',
            f'0x{interior:08X}'],
        'static_dispatch_entry_pcs': [
            f'0x{static_entry:08X}', f'0x{interior:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][runtime_entry] == 'DISPATCH_ENTRY'
    assert runtime_entry not in audit['cross_variant_hosted_demands']
    assert audit['included_reasons'][static_entry] == 'STATIC_DISPATCH_ENTRY'
    assert static_entry in audit['static_exact_fragment_demands']
    assert static_entry in audit['cross_variant_hosted_demands']
    assert f'0x{static_entry:08X}' in seeds
    assert f'call_root 0x{static_entry:08X}' not in seeds
    assert audit['included_reasons'][interior] == 'DISPATCH_INTERIOR'
    assert interior not in audit['cross_variant_hosted_demands']

    malformed = dict(cap, static_dispatch_entry_pcs=[f'0x{LOAD:08X}'])
    try:
        MOD.classify_overlay_seeds(
            malformed, bytes(data), LOAD, len(data), 0, {})
    except RuntimeError as exc:
        assert 'must be a subset' in str(exc)
    else:
        raise AssertionError('static dispatch superset accepted')


def check_owned_direct_call_is_interior():
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = host + 0x08
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x100, 0x24020001)
    put(data, 0x104, 0x24420001)
    put(data, 0x108, 0x24420001)
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{host:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][target] == 'DISPATCH_INTERIOR'
    assert f'interior 0x{target:08X}' in seeds


def check_discovered_host_owns_same_round_call_target():
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = host + 0x08
    put(data, 0x00, jal(host))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, jal(target))
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x03E00008)
    put(data, 0x14, 0x00000000)
    put(data, 0x100, 0x24020001)
    put(data, 0x104, 0x24420001)
    put(data, 0x108, 0x24420001)
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][host] == 'DIRECT_JAL_TARGET'
    assert audit['included_reasons'][target] == 'DISPATCH_INTERIOR'
    assert f'call_root 0x{host:08X}' in seeds
    assert f'interior 0x{target:08X}' in seeds


def check_later_discovered_host_retracts_interior_root():
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = LOAD + 0x120
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x100, 0x10000007)  # b target
    put(data, 0x104, 0x00000000)
    put(data, 0x120, jal(host))
    put(data, 0x124, 0x00000000)
    put(data, 0x128, 0x03E00008)
    put(data, 0x12C, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit['included_reasons'][host] == 'DIRECT_JAL_TARGET'
    assert audit['included_reasons'][target] == 'DISPATCH_INTERIOR'
    assert f'call_root 0x{host:08X}' in seeds
    assert f'interior 0x{target:08X}' in seeds


def check_recompiler_discovered_host_ownership(recompiler):
    data = bytearray(0x200)
    host = LOAD + 0x100
    target = host + 0x08
    put(data, 0x00, jal(host))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, jal(target))
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x03E00008)
    put(data, 0x14, 0x00000000)
    put(data, 0x100, 0x24020001)
    put(data, 0x104, 0x24420001)
    put(data, 0x108, 0x24420001)
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 'discovered_ownership.psx')
        seeds = os.path.join(tmp, 'seeds.txt')
        out = os.path.join(tmp, 'out')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, 'w', encoding='utf-8') as f:
            f.write(f'0x{LOAD:08X}\n')
            f.write(f'interior 0x{target:08X}\n')
        result = subprocess.run(
            [recompiler, psx, '--seeds', seeds, '--out-dir', out, '--overlay'],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob('*_full*.c'))
        ranges = next(pathlib.Path(out).glob('*_full.ranges')).read_text()

    assert f'void func_{host:08X}' in full
    assert f'void func_{target:08X}' in full
    assert f'F {target:08X}' in ranges
    assert f'R {host:08X}' in ranges


def check_recompiler_explicit_hosted_interior(recompiler):
    """A hosted alias must be an organic block leader in the named host."""
    data = bytearray(0x100)
    host = LOAD
    target = LOAD + 0x10
    far_path = LOAD + 0x30

    # The conditional path keeps the host envelope open through far_path.  The
    # fallthrough exits through an unresolved JR, so target is deliberately not
    # exact-walk reachable.  It is nevertheless the organic block after that
    # JR's delay slot -- the indirect-switch case shape this seed is for.
    put(data, 0x00, 0x1480000B)  # bne a0,zero,far_path
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x01000008)  # jr t0
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x24420001)
    put(data, 0x14, 0x03E00008)
    put(data, 0x18, 0x00000000)
    put(data, 0x30, 0x03E00008)
    put(data, 0x34, 0x00000000)

    def run(extra_target):
        with tempfile.TemporaryDirectory() as tmp:
            psx = os.path.join(tmp, 'hosted_interior.psx')
            seeds = os.path.join(tmp, 'seeds.txt')
            out = os.path.join(tmp, 'out')
            with open(psx, 'wb') as f:
                f.write(make_psxexe(host, data))
            with open(seeds, 'w', encoding='utf-8') as f:
                f.write(f'call_root 0x{host:08X}\n')
                f.write(
                    f'hosted_interior 0x{extra_target:08X} 0x{host:08X}\n')
            result = subprocess.run(
                [recompiler, psx, '--seeds', seeds, '--out-dir', out,
                 '--overlay'], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr or result.stdout
            ranges_path = next(pathlib.Path(out).glob('*_full.ranges'))
            identities = MOD.parse_overlay_func_ids(
                str(ranges_path), bytes(data), LOAD, len(data))
            full = ''.join(path.read_text()
                           for path in pathlib.Path(out).glob('*_full*.c'))
            return identities, full, result.stdout + result.stderr

    identities, full, _log = run(target)
    by_entry = {entry: (crc, ranges) for entry, crc, ranges in identities}
    assert target in by_entry
    assert host in by_entry
    assert by_entry[target] == by_entry[host]
    assert f'void func_{target:08X}' in full

    # Four bytes into the same block is not independently enterable.  Naming a
    # valid host does not turn an arbitrary aligned address into an alias.
    bad_target = target + 4
    identities, full, log = run(bad_target)
    assert bad_target not in {entry for entry, _crc, _ranges in identities}
    assert f'void func_{bad_target:08X}' not in full
    assert 'block=no' in log


def check_recompiler_hosted_interior_parser(recompiler):
    data = bytearray(0x80)
    host = LOAD
    target = LOAD + 0x10
    put(data, 0x00, 0x03E00008)
    put(data, 0x04, 0)

    def invoke(lines):
        with tempfile.TemporaryDirectory() as tmp:
            psx = os.path.join(tmp, 'hosted_parser.psx')
            seeds = os.path.join(tmp, 'seeds.txt')
            out = os.path.join(tmp, 'out')
            with open(psx, 'wb') as f:
                f.write(make_psxexe(host, data))
            with open(seeds, 'w', encoding='utf-8') as f:
                f.write('\n'.join(lines) + '\n')
            return subprocess.run(
                [recompiler, psx, '--seeds', seeds, '--out-dir', out,
                 '--overlay'], capture_output=True, text=True)

    invalid_cases = (
        [f'hosted_interiorXYZ 0x{target:08X} 0x{host:08X}'],
        [f'hosted_interior 0x{target + 2:08X} 0x{host:08X}'],
        [f'hosted_interior 0x{target:08X} 0x{host:08X}junk'],
        [f'hosted_interior 0x{target:08X} nope'],
        [f'hosted_interior 0x{LOAD + len(data):08X} 0x{host:08X}'],
        [f'hosted_interior 0x{target:08X} 0x{host:08X}',
         f'hosted_interior 0x{target:08X} 0x{host + 0x20:08X}'],
    )
    for lines in invalid_cases:
        result = invoke(lines)
        assert result.returncode != 0, lines
        assert 'ERROR:' in result.stderr, (lines, result.stdout, result.stderr)


def assert_all_interiors_have_final_host(data, seeds, producer_ranges=(),
                                         allow_cross=True):
    roots = []
    interiors = set()
    for seed in seeds:
        if seed.startswith('interior '):
            interiors.add(int(seed.split()[-1], 16))
        elif seed.startswith(('producer_range ', 'cross_call_allow ',
                              'retained_alias ')):
            continue
        else:
            roots.append(int(seed.split()[-1], 16))
    covered = set()
    roots.sort()
    for index, entry in enumerate(roots):
        hard_cap = roots[index + 1] if index + 1 < len(roots) else LOAD + len(data)
        covered.update(MOD._walk_overlay_function(
            bytes(data), LOAD, len(data), entry, hard_cap, producer_ranges,
            allow_cross)['visited'])
    assert interiors <= covered, (interiors - covered)


def check_ownership_invalidation_and_producer_boundaries():
    # R discovers A+B; expanded A discovers X between them. X caps A before B,
    # so the final partition must reconsider B as a root, not retain a stale
    # A-owned interior classification.
    data = bytearray(0x200)
    host = LOAD + 0x100
    middle = LOAD + 0x120
    target = LOAD + 0x140
    put(data, 0x00, jal(host))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, jal(target))
    put(data, 0x0C, 0x00000000)
    put(data, 0x10, 0x03E00008)
    put(data, 0x14, 0x00000000)
    put(data, 0x100, 0x08000000 | (((LOAD + 0x130) >> 2) & 0x03FFFFFF))
    put(data, 0x104, 0x00000000)
    put(data, 0x120, 0x03E00008)
    put(data, 0x124, 0x00000000)
    put(data, 0x130, 0x24420001)
    put(data, 0x134, 0x24420001)
    put(data, 0x138, 0x24420001)
    put(data, 0x13C, 0x24420001)
    put(data, 0x140, jal(middle))
    put(data, 0x144, 0x00000000)
    put(data, 0x148, 0x03E00008)
    put(data, 0x14C, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    root_addrs = sorted(int(seed.split()[-1], 16) for seed in seeds
                        if not seed.startswith(('interior ', 'producer_range ',
                                                'cross_call_allow ',
                                                'retained_alias ')))
    final_predecessor = max(addr for addr in root_addrs if addr < target)
    assert final_predecessor != host
    assert (audit['included_reasons'][target] != 'DISPATCH_INTERIOR' or
            final_predecessor == LOAD + 0x130)
    assert_all_interiors_have_final_host(data, seeds)

    # A and B straddle producer boundaries. The unapproved cross-producer call
    # is rejected entirely; approval makes B a separate root, never an alias
    # owned by A's apparent fallthrough.
    cross = bytearray(0x200)
    a = LOAD + 0x100
    b = LOAD + 0x108
    put(cross, 0x00, jal(a))
    put(cross, 0x04, 0x00000000)
    put(cross, 0x08, jal(b))
    put(cross, 0x0C, 0x00000000)
    put(cross, 0x10, 0x03E00008)
    put(cross, 0x14, 0x00000000)
    put(cross, 0x100, 0x24420001)
    put(cross, 0x104, 0x24420001)
    put(cross, 0x108, 0x03E00008)
    put(cross, 0x10C, 0x00000000)
    raw_ranges = [
        {'start': f'0x{LOAD:08X}', 'end': f'0x{b:08X}'},
        {'start': f'0x{b:08X}', 'end': f'0x{LOAD + len(cross):08X}'},
    ]
    strict_cap = dict(cap, producer_ranges=raw_ranges,
                      strict_producer_ranges=True)
    strict_seeds, strict_audit = MOD.classify_overlay_seeds(
        strict_cap, bytes(cross), LOAD, len(cross), 0, {})
    assert b not in strict_audit['included_reasons']
    assert b in strict_audit['rejected_cross_producer_calls']
    assert not any(seed.endswith(f'0x{b:08X}') and
                   not seed.startswith('producer_range ')
                   for seed in strict_seeds)
    parsed_ranges = [(LOAD, b), (b, LOAD + len(cross))]
    assert_all_interiors_have_final_host(
        cross, strict_seeds, parsed_ranges, False)

    allowed_cap = dict(cap, producer_ranges=raw_ranges,
                       strict_producer_ranges=False)
    allowed_seeds, allowed_audit = MOD.classify_overlay_seeds(
        allowed_cap, bytes(cross), LOAD, len(cross), 0, {})
    assert allowed_audit['included_reasons'][b] == 'DIRECT_JAL_TARGET'
    assert f'call_root 0x{b:08X}' in allowed_seeds
    assert f'interior 0x{b:08X}' not in allowed_seeds
    assert_all_interiors_have_final_host(
        cross, allowed_seeds, parsed_ranges, True)

    crossing_alias_cap = dict(
        strict_cap,
        static_alias_ranges=[{
            'entry': f'0x{a:08X}',
            'start': f'0x{a:08X}',
            'end': f'0x{b + 4:08X}',
        }])
    try:
        MOD.classify_overlay_seeds(
            crossing_alias_cap, bytes(cross), LOAD, len(cross), 0, {})
        assert False, 'cross-producer supplied alias was accepted'
    except RuntimeError as exc:
        assert 'crosses producer boundary' in str(exc)

    # A seed in composite padding has no producer. It must be rejected rather
    # than letting producer_for(None) equate disconnected uncovered gaps.
    gap = LOAD + 0x60
    gap_data = bytearray(0x100)
    put(gap_data, 0x00, 0x03E00008)
    put(gap_data, 0x04, 0x00000000)
    put(gap_data, 0x60, 0x24020001)
    put(gap_data, 0x64, 0x03E00008)
    put(gap_data, 0x68, 0x00000000)
    gap_cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{gap:08X}'],
        'producer_ranges': [
            {'start': f'0x{LOAD:08X}', 'end': f'0x{LOAD + 0x40:08X}'},
            {'start': f'0x{LOAD + 0x80:08X}',
             'end': f'0x{LOAD + len(gap_data):08X}'},
        ],
        'strict_producer_ranges': True,
    }
    gap_seeds, gap_audit = MOD.classify_overlay_seeds(
        gap_cap, bytes(gap_data), LOAD, len(gap_data), 0, {})
    assert gap not in gap_audit['included_reasons']
    assert not any(seed.endswith(f'0x{gap:08X}') and
                   not seed.startswith('producer_range ')
                   for seed in gap_seeds)

    orphan = LOAD + 0x180
    put(cross, 0x180, 0x24420001)
    orphan_cap = dict(cap, dispatch_entry_pcs=[f'0x{orphan:08X}'])
    orphan_seeds, orphan_audit = MOD.classify_overlay_seeds(
        orphan_cap, bytes(cross), LOAD, len(cross), 0, {})
    assert orphan not in orphan_audit['included_reasons']
    assert f'interior 0x{orphan:08X}' not in orphan_seeds
    assert_all_interiors_have_final_host(cross, orphan_seeds)


def check_recompiler_unreachable_jal_not_alias(recompiler):
    # The host's address envelope contains an unreachable JAL-shaped word whose
    # non-dense target looks like code. Only a raw envelope rescan sees it; the
    # reachable analyzer must not export it as an alias.
    data = bytearray(0x100)
    target = LOAD + 0x40
    put(data, 0x00, 0x08000000 | (((LOAD + 0x80) >> 2) & 0x03FFFFFF))
    put(data, 0x04, 0x00000000)
    put(data, 0x20, jal(target))
    put(data, 0x40, 0x24420001)
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)
    put(data, 0x80, 0x03E00008)
    put(data, 0x84, 0x00000000)
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 'unreachable_jal.psx')
        seeds = os.path.join(tmp, 'seeds.txt')
        out = os.path.join(tmp, 'out')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, 'w', encoding='utf-8') as f:
            f.write(f'0x{LOAD:08X}\n')
            f.write(f'interior 0x{target:08X}\n')
        result = subprocess.run(
            [recompiler, psx, '--seeds', seeds, '--out-dir', out, '--overlay'],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob('*_full*.c'))
    assert f'void func_{target:08X}' not in full


def check_recomputed_evidence_and_unconditional_branch():
    # R reaches S, S calls B, and B calls earlier X. Adding X caps R before S,
    # invalidating the only source edge for B. This oscillating partition must
    # fail closed to explicit R rather than retain monotonic B evidence.
    data = bytearray(0x200)
    x = LOAD + 0x80
    s = LOAD + 0x100
    b = LOAD + 0x140
    put(data, 0x00, 0x08000000 | ((s >> 2) & 0x03FFFFFF))
    put(data, 0x04, 0x00000000)
    put(data, 0x80, 0x03E00008)
    put(data, 0x84, 0x00000000)
    put(data, 0x100, jal(b))
    put(data, 0x104, 0x00000000)
    put(data, 0x108, 0x03E00008)
    put(data, 0x10C, 0x00000000)
    put(data, 0x140, jal(x))
    put(data, 0x144, 0x00000000)
    put(data, 0x148, 0x03E00008)
    put(data, 0x14C, 0x00000000)
    cap = {'schema': 'psxrecomp overlay capture v2',
           'function_entry_pcs': [f'0x{LOAD:08X}']}
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert b not in audit['included_reasons']
    assert not any(seed.endswith(f'0x{b:08X}') for seed in seeds)

    # Always-taken beq rN,rN has no PC+8 fallthrough. A JAL-shaped word
    # there is unreachable and cannot manufacture a root or alias.
    branch = bytearray(0x100)
    hidden = LOAD + 0x40
    put(branch, 0x00, (0x04 << 26) | (8 << 21) | (8 << 16) | 0x1F)
    put(branch, 0x04, 0x00000000)
    put(branch, 0x08, jal(hidden))
    put(branch, 0x0C, 0x00000000)
    put(branch, 0x40, 0x03E00008)
    put(branch, 0x44, 0x00000000)
    put(branch, 0x80, 0x03E00008)
    put(branch, 0x84, 0x00000000)
    branch_seeds, branch_audit = MOD.classify_overlay_seeds(
        cap, bytes(branch), LOAD, len(branch), 0, {})
    assert hidden not in branch_audit['included_reasons']
    assert not any(seed.endswith(f'0x{hidden:08X}') for seed in branch_seeds)

    # Never-taken bne rN,rN must not walk its encoded target.
    never = bytearray(0x100)
    put(never, 0x00, (0x05 << 26) | (8 << 21) | (8 << 16) | 0x1F)
    put(never, 0x04, 0x00000000)
    put(never, 0x08, 0x03E00008)
    put(never, 0x0C, 0x00000000)
    put(never, 0x40, 0x03E00008)
    put(never, 0x44, 0x00000000)
    put(never, 0x80, jal(hidden))
    put(never, 0x84, 0x00000000)
    put(never, 0x88, 0x03E00008)
    put(never, 0x8C, 0x00000000)
    never_seeds, never_audit = MOD.classify_overlay_seeds(
        cap, bytes(never), LOAD, len(never), 0, {})
    assert hidden not in never_audit['included_reasons']
    assert not any(seed.endswith(f'0x{hidden:08X}') for seed in never_seeds)

    # All zero-provable branch families use exact direction. Likely-false
    # variants additionally annul their delay slot.
    target = LOAD + 0x80
    assert MOD._classify_cf(LOAD, (0x06 << 26) | 0x1F) == ('j', target)
    assert MOD._classify_cf(LOAD, (0x07 << 26) | 0x1F) == ('branch_never', 0)
    assert MOD._classify_cf(LOAD, (0x01 << 26) | (0x00 << 16) | 0x1F) == (
        'branch_never', 0)  # bltz zero
    assert MOD._classify_cf(LOAD, (0x01 << 26) | (0x01 << 16) | 0x1F) == (
        'j', target)  # bgez zero
    assert MOD._classify_cf(LOAD, (0x01 << 26) | (0x11 << 16) | 0x1F) == (
        'jal', target)  # bal keeps both target and return continuation
    assert MOD._classify_cf(LOAD, (0x15 << 26) | (8 << 21) |
                            (8 << 16) | 0x1F) == ('branch_never_likely', 0)
    assert MOD._classify_cf(LOAD, (0x17 << 26) | 0x1F) == (
        'branch_never_likely', 0)

    # A non-canonical collection of branch-shaped words remains unresolved.
    assert MOD._find_jump_table_targets(
        bytes(branch), LOAD, len(branch), LOAD, LOAD + len(branch),
        LOAD + 0x20, 8) == set()

    # A jump enters at the low-half definition, skipping the preceding LUI.
    # The backward resolver must not combine definitions across that inbound
    # basic-block boundary.
    split = bytearray(0x600)
    put(split, 0x400, 0x03E00008)
    put(split, 0x404, 0x00000000)
    put(split, 0x500, 0x08000000 | (((LOAD + 0x518) >> 2) & 0x03FFFFFF))
    put(split, 0x504, 0x00000000)
    put(split, 0x514, 0x3C088001)
    put(split, 0x518, 0x25080400)
    put(split, 0x51C, 0x01000008)
    put(split, 0x520, 0x00000000)
    split_walk = MOD._walk_overlay_function(
        bytes(split), LOAD, len(split), LOAD + 0x500, LOAD + len(split))
    assert split_walk['static_indirect_targets'] == set()

    # A LUI in a JAL delay slot can be clobbered by the callee before control
    # returns to the low half. Raw adjacency is not reaching-definition proof.
    call_delay = bytearray(0x600)
    put(call_delay, 0x300, 0x03E00008)
    put(call_delay, 0x304, 0x00000000)
    put(call_delay, 0x400, 0x03E00008)
    put(call_delay, 0x404, 0x00000000)
    put(call_delay, 0x500, jal(LOAD + 0x300))
    put(call_delay, 0x504, 0x3C088001)
    put(call_delay, 0x508, 0x25080400)
    put(call_delay, 0x50C, 0x0100F809)
    put(call_delay, 0x510, 0x00000000)
    put(call_delay, 0x514, 0x03E00008)
    put(call_delay, 0x518, 0x00000000)
    call_delay_walk = MOD._walk_overlay_function(
        bytes(call_delay), LOAD, len(call_delay), LOAD + 0x500,
        LOAD + len(call_delay))
    assert call_delay_walk['direct_jals'] == {LOAD + 0x300}
    assert call_delay_walk['static_indirect_targets'] == set()


def check_t2_shaped_retained_partition_conflict(recompiler):
    # Shape of the causal T2 regression: current ordinary host BA0 ends +0x114,
    # new ordinary roots exist at +0x17C/+0x180, while an old retained alias
    # claims the original host through +0x2A8. The old range must be ignored.
    data = bytearray(0x300)
    old_alias = LOAD + 0x114
    split_a = LOAD + 0x17C
    split_b = LOAD + 0x180
    put(data, 0x10C, 0x03E00008)
    put(data, 0x110, 0x00000000)
    put(data, 0x17C, 0x24420001)
    put(data, 0x180, 0x03E00008)
    put(data, 0x184, 0x00000000)
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 't2_partition_conflict.psx')
        seed_path = os.path.join(tmp, 'seeds.txt')
        out = os.path.join(tmp, 'out')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(LOAD, data))
        with open(seed_path, 'w', encoding='utf-8') as f:
            f.write(f'0x{LOAD:08X}\n')
            f.write(f'call_root 0x{split_a:08X}\n')
            f.write(f'call_root 0x{split_b:08X}\n')
            f.write(f'retained_alias 0x{old_alias:08X} '
                    f'0x{LOAD:08X} 0x{LOAD + 0x2A8:08X}\n')
        result = subprocess.run(
            [recompiler, psx, '--seeds', seed_path, '--out-dir', out,
             '--overlay'], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        ranges = next(pathlib.Path(out).glob('*_full.ranges')).read_text()
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob('*_full*.c'))
    assert 'ignoring legacy retained_alias' in result.stdout
    assert f'F {old_alias:08X}' not in ranges
    assert f'void func_{old_alias:08X}' not in full
    assert f'F {split_a:08X}' in ranges
    assert f'F {split_b:08X}' in ranges


def check_static_alias_recipe():
    data = bytearray(0x40)
    put(data, 0x00, 0x03E00008)
    put(data, 0x04, 0x00000000)
    put(data, 0x20, 0x24020001)
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{LOAD + 0x20:08X}'],
        'static_alias_ranges': [{
            'entry': f'0x{LOAD + 0x20:08X}',
            'start': f'0x{LOAD:08X}',
            'end': f'0x{LOAD + 0x2C:08X}',
        }],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert not any(seed.startswith('retained_alias ') for seed in seeds)
    assert f'interior 0x{LOAD + 0x20:08X}' not in seeds
    assert LOAD + 0x20 not in audit['included_reasons']
    assert audit['static_alias_ranges'] == {
        (LOAD + 0x20, LOAD, LOAD + 0x2C)}
    assert audit['static_interval_fragment_demands'] == {LOAD + 0x20}


def check_optional_enrichment_fallback():
    direct = LOAD + 0x20
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{direct:08X}'],
        'dispatch_entry_pcs': [f'0x{LOAD:08X}', f'0x{direct:08X}'],
        'static_discovery_entry_pcs': [f'0x{direct:08X}'],
        'seeds': [f'0x{LOAD:08X}', f'0x{direct:08X}'],
        'static_alias_ranges': [{
            'entry': f'0x{direct:08X}',
            'start': f'0x{LOAD:08X}',
            'end': f'0x{LOAD + 0x2C:08X}',
        }],
        '_prior_aliases': [(direct, LOAD, LOAD + 0x2C)],
        'static_jump_table_proofs': [{'host_entry': f'0x{LOAD:08X}'}],
        'static_call_continuation_pcs': [f'0x{LOAD + 0x10:08X}'],
        'optional_enrichment_fallback_entry_pcs': [f'0x{direct:08X}'],
    }
    fallback = MOD.optional_enrichment_fallback_capture(cap)
    encoded = [f'0x{direct:08X}']
    assert fallback['function_entry_pcs'] == encoded
    assert fallback['dispatch_entry_pcs'] == encoded
    assert fallback['static_dispatch_entry_pcs'] == []
    assert fallback['static_discovery_entry_pcs'] == encoded
    assert fallback['seeds'] == encoded
    assert 'static_alias_ranges' not in fallback
    assert 'static_jump_table_proofs' not in fallback
    assert 'static_call_continuation_pcs' not in fallback
    assert '_prior_aliases' not in fallback
    assert 'optional_enrichment_fallback_entry_pcs' not in fallback
    assert MOD.optional_enrichment_fallback_capture(fallback) is None

    dispatch = LOAD + 0x30
    cap['optional_enrichment_fallback_entry_pcs'] = {
        'function_entry_pcs': [],
        'dispatch_entry_pcs': [f'0x{dispatch:08X}'],
        'static_dispatch_entry_pcs': [f'0x{dispatch:08X}'],
        'static_discovery_entry_pcs': [],
        'producer_ranges': [{
            'start': f'0x{LOAD + 0x20:08X}',
            'end': f'0x{LOAD + 0x40:08X}',
        }],
        'strict_producer_ranges': True,
    }
    fallback = MOD.optional_enrichment_fallback_capture(cap)
    assert fallback['function_entry_pcs'] == []
    assert fallback['dispatch_entry_pcs'] == [f'0x{dispatch:08X}']
    assert fallback['static_dispatch_entry_pcs'] == [f'0x{dispatch:08X}']
    assert fallback['static_discovery_entry_pcs'] == []
    assert fallback['seeds'] == []
    assert fallback['producer_ranges'] == cap[
        'optional_enrichment_fallback_entry_pcs']['producer_ranges']
    assert fallback['strict_producer_ranges'] is True

    # Exercise the real bounded extractor recipe through both downstream
    # transforms. Prologue-host static provenance must survive sanitization;
    # otherwise sibling nomination silently loses its only durable authority.
    bounded = bytearray(0x90)
    for off in (0x10, 0x40, 0x70):
        put(bounded, off, 0x27BDFFF0)
        put(bounded, off + 4, 0x03E00008)
        put(bounded, off + 8, 0x27BD0010)
    bounded_recipe = EXTRACT.bounded_dispatch_fallback(
        bytes(bounded), LOAD, [LOAD + 0x24, LOAD + 0x50])
    wrapped = {
        'schema': 'psxrecomp overlay capture v2',
        'load_addr': f'0x{LOAD:08X}',
        'size': len(bounded),
        'optional_enrichment_fallback_entry_pcs': bounded_recipe,
    }
    bounded_fallback = MOD.optional_enrichment_fallback_capture(wrapped)
    assert bounded_fallback['static_dispatch_entry_pcs'] == \
        bounded_recipe['static_dispatch_entry_pcs']
    assert set(bounded_fallback['static_dispatch_entry_pcs']) <= set(
        bounded_fallback['dispatch_entry_pcs'])
    _seeds, bounded_audit = MOD.classify_overlay_seeds(
        bounded_fallback, bytes(bounded), LOAD, len(bounded), 0, {})
    for host in (LOAD + 0x10, LOAD + 0x40):
        assert bounded_audit['included_reasons'][host] == \
            'STATIC_DISPATCH_ENTRY'
        assert host in bounded_audit['cross_variant_hosted_demands']


def check_forward_branch_root():
    data = bytearray(0x80)
    target = LOAD + 0x40
    put(data, 0x00, 0x1000000F)  # beq zero,zero,target
    put(data, 0x04, 0x00000000)
    put(data, 0x20, 0x27BDFFF0)  # sibling root that hard-caps the first walk
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x27BD0010)
    put(data, 0x40, 0x24020001)  # forward branch target, frameless
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)
    cap = {
        'schema': 'psxrecomp overlay capture v2',
        'function_entry_pcs': [f'0x{LOAD:08X}', f'0x{LOAD + 0x20:08X}'],
        'static_discovery_entry_pcs': [
            f'0x{LOAD:08X}', f'0x{LOAD + 0x20:08X}'],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert f'call_root 0x{target:08X}' in seeds
    assert audit['included_reasons'][target] == 'STATIC_BRANCH_ROOT'


def check_padded_return_boundary(recompiler):
    # Exact-entry mode re-verifies every untrusted seed. Psy-Q aligns frameless
    # leaves with NOPs after the prior function's JR delay slot; those NOPs must
    # not erase the otherwise exact return-boundary evidence.
    data = bytearray(0x80)
    put(data, 0x00, 0x03E00008)  # jr $ra
    put(data, 0x04, 0x24020007)  # non-NOP delay slot
    # 0x08..0x10 are alignment NOPs.
    put(data, 0x14, 0x24020001)  # padded frameless leaf
    put(data, 0x18, 0x03E00008)
    put(data, 0x1C, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "padded.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\n0x{LOAD + 0x14:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = next(pathlib.Path(out).glob("*_full.c")).read_text()
        assert f"void func_{LOAD + 0x14:08X}" in full
        ranges = next(pathlib.Path(out).glob("*_full.ranges")).read_text()
        assert f"F {LOAD + 0x14:08X}" in ranges


def check_recompiler_composite_contract(recompiler):
    data = bytearray(0x80)
    target = LOAD + 0x50
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    put(data, 0x50, 0x24020001)
    put(data, 0x54, 0x03E00008)
    put(data, 0x58, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "composite.psx")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))

        def generated(allow):
            seeds = os.path.join(tmp, f"seeds_{allow}.txt")
            out = os.path.join(tmp, f"out_{allow}")
            with open(seeds, "w", encoding="utf-8") as f:
                f.write(f"producer_range 0x{LOAD:08X} 0x{LOAD + 0x40:08X}\n")
                f.write(f"producer_range 0x{LOAD + 0x40:08X} 0x{LOAD + 0x80:08X}\n")
                if allow:
                    f.write(f"cross_call_allow 0x{target:08X}\n")
                f.write(f"0x{LOAD:08X}\n")
            result = subprocess.run(
                [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
                capture_output=True, text=True)
            assert result.returncode == 0, result.stderr or result.stdout
            return ''.join(path.read_text() for path in pathlib.Path(out).glob("*_full*.c"))

        assert f"void func_{target:08X}" not in generated(False)
        assert f"void func_{target:08X}" in generated(True)


def check_recompiler_pointer_table_call_root(recompiler):
    data = bytearray(0x100)
    target = LOAD + 0x50
    put(data, 0x00, jal(target))
    put(data, 0x04, 0x00000000)
    put(data, 0x08, 0x03E00008)
    put(data, 0x0C, 0x00000000)
    for index, offset in enumerate((0x70, 0x80, 0x90)):
        put(data, 0x50 + index * 4, LOAD + offset)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "pointer_table.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\ncall_root 0x{target:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob("*_full*.c"))
        assert f"void func_{target:08X}" not in full


def check_recompiler_pointer_table_alias(recompiler):
    # A function envelope may span unreachable data between a jump and its
    # target.  A JAL-shaped word in that hole must not turn a dense pointer
    # table inside the envelope into an alias function.
    data = bytearray(0x100)
    target = LOAD + 0x40
    put(data, 0x00, 0x08000000 | (((LOAD + 0x80) >> 2) & 0x03FFFFFF))
    put(data, 0x04, 0x00000000)
    put(data, 0x20, jal(target))
    for index, offset in enumerate((0x70, 0x80, 0x90)):
        put(data, 0x40 + index * 4, LOAD + offset)
    put(data, 0x80, 0x03E00008)
    put(data, 0x84, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "pointer_table_alias.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text()
                       for path in pathlib.Path(out).glob("*_full*.c"))
        assert "+1 in-function jal-target alias entries" not in result.stdout
        assert f"void func_{target:08X}" not in full


def check_retained_alias_contract(recompiler):
    # Legacy host ranges are ignored. A prior alias can be reintroduced only as
    # an entry candidate and must acquire a host from current exact reachability.
    data = bytearray(0x80)
    put(data, 0x00, 0x03E00008)
    put(data, 0x04, 0x00000000)
    put(data, 0x20, 0x24020001)
    put(data, 0x24, 0x03E00008)
    put(data, 0x28, 0x00000000)

    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "retained_alias.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, data))
        with open(seeds, "w", encoding="utf-8") as f:
            f.write(f"0x{LOAD:08X}\n")
            f.write(
                f"retained_alias 0x{LOAD + 0x20:08X} "
                f"0x{LOAD:08X} 0x{LOAD + 0x2C:08X}\n")
        result = subprocess.run(
            [recompiler, psx, "--seeds", seeds, "--out-dir", out, "--overlay"],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr or result.stdout
        full = ''.join(path.read_text() for path in pathlib.Path(out).glob("*_full*.c"))
        assert f"void func_{LOAD + 0x20:08X}" not in full
        ranges = next(pathlib.Path(out).glob("*_full.ranges")).read_text()
        assert f"F {LOAD + 0x20:08X}" not in ranges


def check_atomic_dll_publication():
    original = MOD._compile_dll_direct
    original_replace = MOD.os.replace
    original_pair_valid = MOD._runtime_valid_shard_pair_locked
    try:
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "shard.dll")
            ranges = os.path.join(tmp, "shard.ranges")
            with open(final, "wb") as out:
                out.write(b"old-good-shard")
            with open(ranges, "w", encoding="utf-8") as out:
                out.write("OLD-RANGES\n")

            def fail_compile(_src, staged, _includes, **_kwargs):
                with open(staged, "wb") as out:
                    out.write(b"partial")
                with open(os.path.splitext(staged)[0] + ".def", "w") as out:
                    out.write("temporary exports")
                return False

            MOD._compile_dll_direct = fail_compile
            assert not MOD.compile_dll("ignored.c", final, [])
            with open(final, "rb") as built:
                assert built.read() == b"old-good-shard"
            assert pathlib.Path(ranges).read_text() == "OLD-RANGES\n"
            assert not list(pathlib.Path(tmp).glob(".*.tmp.*"))
            assert MOD.shard_pair_files_complete(final)

            def good_compile(_src, staged, _includes, **_kwargs):
                with open(staged, "wb") as out:
                    out.write(b"new-complete-shard")
                with open(os.path.splitext(staged)[0] + ".def", "w") as out:
                    out.write("temporary exports")
                return True

            MOD._compile_dll_direct = good_compile
            func_ids = [(0x80010000, 0xDEADBEEF,
                         ((0x80010000, 0x10),))]
            pair_id = MOD.overlay_pair_id("new source", func_ids)
            assert pair_id != MOD.overlay_pair_id("changed source", func_ids)
            bound_source = MOD.add_overlay_pair_export("new source", pair_id)
            assert f"overlay_pair_id(void) {{ return UINT64_C(0x{pair_id:016X}); }}" in bound_source
            assert MOD.compile_dll("ignored.c", final, [],
                                   func_ids=func_ids, pair_id=pair_id)
            with open(final, "rb") as built:
                assert built.read() == b"new-complete-shard"
            manifest = pathlib.Path(ranges).read_text()
            assert f"P {pair_id:016X}\n" in manifest
            assert "F 80010000 DEADBEEF\nR 80010000 10\n" in manifest
            assert MOD.shard_pair_files_complete(final)
            assert not list(pathlib.Path(tmp).glob(".*.tmp.*"))

        # A valid first publisher owns the canonical region-CRC name. A later
        # process with different discovery evidence reports preservation and
        # cannot overwrite the winner with its independently generated pair.
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "00010000_DEADBEEF.dll")
            ranges = os.path.splitext(final)[0] + ".ranges"
            pathlib.Path(final).write_bytes(b"first-winner")
            pathlib.Path(ranges).write_text("FIRST-MANIFEST\n")
            MOD._compile_dll_direct = good_compile
            MOD._runtime_valid_shard_pair_locked = (
                lambda _dll, _ranges, _abi: True)
            publication = {}
            assert MOD.compile_dll(
                "ignored.c", final, [], func_ids=func_ids, pair_id=pair_id,
                preserve_existing_pair=True,
                publication_result=publication)
            assert publication == {"published": False}
            assert pathlib.Path(final).read_bytes() == b"first-winner"
            assert pathlib.Path(ranges).read_text() == "FIRST-MANIFEST\n"
            resident_cap = {
                "producer": MOD.BIOS_RESIDENT_PRODUCER,
                "bios_sha256": "ab" * 32,
                "producer_name": "test resident",
            }
            marker = pathlib.Path(final).with_suffix('.resident')
            MOD.reconcile_bios_resident_marker(final, resident_cap, True)
            resident_payload = marker.read_text()
            MOD.reconcile_bios_resident_marker(
                final, {"producer": "ordinary"}, False)
            assert marker.read_text() == resident_payload
            marker.unlink()
            MOD.reconcile_bios_resident_marker(final, resident_cap, False)
            assert marker.exists()  # crash-after-pair self-heal
            MOD.reconcile_bios_resident_marker(
                final, {"producer": "ordinary"}, True)
            assert marker.read_text() == resident_payload
            assert not list(pathlib.Path(tmp).glob(".*.tmp.*"))
            MOD._runtime_valid_shard_pair_locked = original_pair_valid

        # Kill a separate writer process AFTER each durable rename. This tests
        # real kernel lock release and recovery, including the hardest point:
        # the final DLL rename committed but journal cleanup never ran.
        module_path = str(ROOT / "tools" / "compile_overlays.py")
        crash_script = r'''
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("co_child", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
real = m.os.replace
count = 0
def crash_after(src, dst):
    global count
    result = real(src, dst)
    count += 1
    if count == int(sys.argv[5]): os._exit(91)
    return result
m.os.replace = crash_after
m.publish_shard_pair(sys.argv[2], sys.argv[3], sys.argv[4])
'''
        for fail_after in range(1, 6):
            with tempfile.TemporaryDirectory() as tmp:
                final = os.path.join(tmp, "shard.dll")
                ranges = os.path.join(tmp, "shard.ranges")
                staged = os.path.join(tmp, ".new.dll")
                staged_ranges = os.path.join(tmp, ".new.ranges")
                pathlib.Path(final).write_bytes(b"OLD-DLL")
                pathlib.Path(ranges).write_text("OLD-RANGES\n")
                pathlib.Path(staged).write_bytes(b"NEW-DLL")
                pathlib.Path(staged_ranges).write_text("NEW-RANGES\n")
                child = subprocess.run(
                    [sys.executable, "-c", crash_script, module_path,
                     staged, staged_ranges, final, str(fail_after)])
                assert child.returncode == 91

                MOD.recover_shard_pair(final)
                expected = ((b"NEW-DLL", "NEW-RANGES\n") if fail_after == 5
                            else (b"OLD-DLL", "OLD-RANGES\n"))
                assert pathlib.Path(final).read_bytes() == expected[0]
                assert pathlib.Path(ranges).read_text() == expected[1]
                assert not list(pathlib.Path(tmp).glob("*.pair-txn.json"))

        # Recovery is restartable if an individual rollback rename faults.
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "shard.dll")
            ranges = os.path.join(tmp, "shard.ranges")
            staged = os.path.join(tmp, ".new.dll")
            staged_ranges = os.path.join(tmp, ".new.ranges")
            pathlib.Path(final).write_bytes(b"OLD-DLL")
            pathlib.Path(ranges).write_text("OLD-RANGES\n")
            pathlib.Path(staged).write_bytes(b"NEW-DLL")
            pathlib.Path(staged_ranges).write_text("NEW-RANGES\n")
            child = subprocess.run(
                [sys.executable, "-c", crash_script, module_path,
                 staged, staged_ranges, final, "3"])
            assert child.returncode == 91
            real_replace = MOD.os.replace
            failed_once = False

            def fail_one_restore(src, dst):
                nonlocal failed_once
                if not failed_once and str(src).endswith('.old-ranges'):
                    failed_once = True
                    raise OSError("injected recovery rename fault")
                return real_replace(src, dst)

            MOD.os.replace = fail_one_restore
            try:
                try:
                    MOD.recover_shard_pair(final)
                    raise AssertionError("recovery fault was not surfaced")
                except OSError as exc:
                    assert "injected recovery" in str(exc)
            finally:
                MOD.os.replace = real_replace
            MOD.recover_shard_pair(final)
            assert pathlib.Path(final).read_bytes() == b"OLD-DLL"
            assert pathlib.Path(ranges).read_text() == "OLD-RANGES\n"

        # Real cross-process writers must serialize on the permanent OS lock.
        publish_script = r'''
import importlib.util, sys
spec = importlib.util.spec_from_file_location("co_child", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
m.publish_shard_pair(sys.argv[2], sys.argv[3], sys.argv[4])
'''
        with tempfile.TemporaryDirectory() as tmp:
            final = os.path.join(tmp, "shard.dll")
            ranges = os.path.join(tmp, "shard.ranges")
            pathlib.Path(final).write_bytes(b"DLL-old")
            pathlib.Path(ranges).write_text("RANGES-old")
            writers = []
            for index in range(8):
                staged = os.path.join(tmp, f"new-{index}.dll")
                staged_ranges = os.path.join(tmp, f"new-{index}.ranges")
                pathlib.Path(staged).write_bytes(f"DLL-{index}".encode())
                pathlib.Path(staged_ranges).write_text(f"RANGES-{index}")
                writers.append(subprocess.Popen(
                    [sys.executable, "-c", publish_script, module_path,
                     staged, staged_ranges, final]))
            assert all(writer.wait(timeout=30) == 0 for writer in writers)
            dll_generation = pathlib.Path(final).read_text().removeprefix("DLL-")
            range_generation = pathlib.Path(ranges).read_text().removeprefix("RANGES-")
            assert dll_generation == range_generation
            assert not list(pathlib.Path(tmp).glob("*.pair-txn.json"))

        # Readers must not observe the deliberate manifest-only publication
        # window. Pause a real publisher after rename #4 while it owns the OS
        # lock; completeness and coverage readers must block, then consume the
        # committed pair rather than the transient half-state.
        pause_script = r'''
import importlib.util, os, pathlib, sys, time
spec = importlib.util.spec_from_file_location("co_child", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
real = m.os.replace
count = 0
def pause_after_manifest(src, dst):
    global count
    result = real(src, dst); count += 1
    if count == 4:
        pathlib.Path(sys.argv[5]).write_text("paused")
        time.sleep(1.0)
    return result
m.os.replace = pause_after_manifest
m.publish_shard_pair(sys.argv[2], sys.argv[3], sys.argv[4])
'''
        for reader in ('complete', 'coverage'):
            with tempfile.TemporaryDirectory() as tmp:
                final = os.path.join(tmp, "00010000_DEADBEEF.dll")
                ranges = os.path.splitext(final)[0] + '.ranges'
                staged = os.path.join(tmp, ".new.dll")
                staged_ranges = os.path.join(tmp, ".new.ranges")
                paused = os.path.join(tmp, "paused")
                pathlib.Path(final).write_bytes(b"OLD-DLL")
                pathlib.Path(ranges).write_text(
                    "F 80010004 AAAAAAAA\nR 80010004 4\n")
                pathlib.Path(staged).write_bytes(b"NEW-DLL")
                pathlib.Path(staged_ranges).write_text(
                    "F 80010000 DEADBEEF\nR 80010000 10\n")
                writer = subprocess.Popen(
                    [sys.executable, "-c", pause_script, module_path,
                     staged, staged_ranges, final, paused])
                deadline = time.monotonic() + 10
                while not os.path.exists(paused) and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert os.path.exists(paused), "publisher did not reach pause point"
                started = time.monotonic()
                if reader == 'complete':
                    assert MOD.shard_pair_files_complete(final)
                else:
                    # Both synthetic DLLs are intentionally unloadable; the
                    # coverage reader must still block through publication and
                    # then reject the completed invalid winner.
                    assert MOD.load_region_coverage(tmp, 0x10000) == set()
                elapsed = time.monotonic() - started
                assert elapsed >= 0.6, f'{reader} escaped pair lock in {elapsed:.3f}s'
                assert writer.wait(timeout=10) == 0

        if os.name == 'nt':
            # Windows keeps a loaded image mapped even after its canonical name
            # is renamed. Old-backup deletion may therefore return WinError 5;
            # a committed new pair must survive and cleanup must be deferred.
            import _ctypes
            import ctypes
            import shutil
            gcc = r'C:\msys64\mingw64\bin\gcc.exe'
            assert os.path.isfile(gcc), "real loaded-DLL regression needs MinGW gcc"
            with tempfile.TemporaryDirectory() as tmp:
                # The actual host compiler must export the same 64-bit identity
                # serialized in P; this catches width/decorated-name mistakes
                # that a source-string assertion cannot.
                pair_id = 0xFEDCBA9876543210
                pair_source = pathlib.Path(tmp) / "pair.c"
                pair_dll = str(pathlib.Path(tmp) / "pair.dll")
                pair_source.write_text(
                    '#include <stdint.h>\n' +
                    '__declspec(dllexport) int overlay_abi(void) { return 14; }\n' +
                    '__declspec(dllexport) void overlay_init(const void *p) {(void)p;}\n' +
                    '__declspec(dllexport) void overlay_flush_cycles(void) {}\n' +
                    '__declspec(dllexport) void func_80010000(void *p) {(void)p;}\n' +
                    MOD.add_overlay_pair_export('', pair_id))
                subprocess.run([gcc, '-shared', str(pair_source), '-o', pair_dll],
                               check=True, capture_output=True, text=True)
                pair_func_ids = [(0x80010000, 0xDEADBEEF,
                                  [(0x80010000, 0x10)])]
                pathlib.Path(pair_dll).with_suffix('.ranges').write_text(
                    MOD.overlay_ranges_text(pair_func_ids, pair_id))
                pair_lib = ctypes.WinDLL(pair_dll)
                pair_fn = pair_lib.overlay_pair_id
                pair_fn.restype = ctypes.c_uint64
                assert pair_fn() == pair_id
                manifest = MOD.overlay_ranges_text(pair_func_ids, pair_id)
                assert f'P {pair_fn():016X}\n' in manifest
                pair_handle = pair_lib._handle
                pair_lib._handle = 0
                _ctypes.FreeLibrary(pair_handle)
                assert MOD.compiled_shard_complete(pair_dll, 14)
                assert not MOD.compiled_shard_complete(
                    pair_dll, 14 | (1 << 16))  # same cg dir, stale flavor
                assert MOD.cached_shard_manifest_status(
                    pair_dll, 14, manifest, pair_id) == 'match'
                assert MOD.cached_shard_manifest_status(
                    pair_dll, 14 | (1 << 16), manifest, pair_id) == 'missing'
                assert MOD.cached_shard_manifest_status(
                    pair_dll, 14, manifest, pair_id ^ 1) == 'mismatch'

                source = pathlib.Path(tmp) / "loaded.c"
                final = str(pathlib.Path(tmp) / "shard.dll")
                ranges = str(pathlib.Path(tmp) / "shard.ranges")
                staged = str(pathlib.Path(tmp) / ".new.dll")
                staged_ranges = str(pathlib.Path(tmp) / ".new.ranges")
                source.write_text("__declspec(dllexport) int loaded(void) { return 7; }\n")
                subprocess.run([gcc, '-shared', str(source), '-o', final],
                               check=True, capture_output=True, text=True)
                pathlib.Path(ranges).write_text("OLD-RANGES\n")
                shutil.copyfile(final, staged)
                pathlib.Path(staged_ranges).write_text("NEW-RANGES\n")
                loaded = ctypes.WinDLL(final)
                MOD.publish_shard_pair(staged, staged_ranges, final)
                assert pathlib.Path(ranges).read_text() == "NEW-RANGES\n"
                assert not os.path.exists(final + '.pair-txn.json')
                deferred = list(pathlib.Path(tmp).glob(
                    'shard.dll.pair-txn.*.old-dll'))
                assert deferred, "loaded old DLL backup should require deferred cleanup"
                handle = loaded._handle
                loaded._handle = 0
                _ctypes.FreeLibrary(handle)
                MOD.recover_shard_pair(final)
                assert not [path for path in deferred if path.exists()]

        with tempfile.TemporaryDirectory() as tmp:
            ranges = pathlib.Path(tmp) / "00010000_DEADBEEF.ranges"
            ranges.write_text("F 80010000 DEADBEEF\nR 80010000 10\n")
            assert MOD.load_region_coverage(tmp, 0x10000) == set()
            ranges.with_suffix(MOD.overlay_ext()).write_bytes(b"DLL")
            # A nonempty but unpaired/non-loadable cache artifact must never
            # suppress recompilation; the runtime would reject it too.
            assert MOD.load_region_coverage(tmp, 0x10000) == set()
    finally:
        MOD.os.replace = original_replace
        MOD._compile_dll_direct = original
        MOD._runtime_valid_shard_pair_locked = original_pair_valid


def check_candidate_capacity_publication():
    """Candidate accounting dedups only exact, same-tier whole pairs."""
    original_exports = MOD._dll_runtime_exports_match
    original_abi = MOD._dll_abi_matches
    try:
        MOD._dll_runtime_exports_match = (
            lambda _path, _abi, _pair, entries: bool(entries))

        def funcs(count, base=LOAD):
            return [
                (base + index * 4, index + 1,
                 ((base + index * 4, 4),))
                for index in range(count)
            ]

        def pair(path, count, pair_id, base=LOAD, provenance=None):
            pathlib.Path(path).write_bytes(b'DLL')
            pathlib.Path(path).with_suffix('.ranges').write_text(
                MOD.overlay_ranges_text(
                    funcs(count, base), pair_id, provenance))

        def pair_records(path, records, pair_id, provenance=None):
            pathlib.Path(path).write_bytes(b'DLL')
            pathlib.Path(path).with_suffix('.ranges').write_text(
                MOD.overlay_ranges_text(records, pair_id, provenance))

        def staged_identity(path):
            manifest = pathlib.Path(path).with_suffix('.ranges').read_text()
            pair_id, manifest_funcs = MOD.parse_runtime_shard_manifest(
                manifest, require_pair=False)
            return MOD._normalized_runtime_manifest_identity(
                manifest, pair_id, manifest_funcs)

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            duplicate = os.path.join(tmp, '00010000_00000002.dll')
            pair(first, 2, 0x100)
            pair(duplicate, 2, 0x100)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 2
            assert counts == {first: 2, duplicate: 2}
            assert counts.raw_total == 4 and counts.file_total == 2
            assert counts.range_link_total == 4

            # Same F rows with a distinct P are not the same compiled pair.
            different_pair = os.path.join(tmp, '00010000_00000003.dll')
            pair(different_pair, 2, 0x101)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 4

            # Nor does P alone override an exact manifest mismatch.
            different_manifest = os.path.join(tmp, '00010000_00000004.dll')
            pair(different_manifest, 1, 0x100, LOAD + 0x40)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 5

            # Provenance is safety-significant even when P/F/R match.
            supplemental = os.path.join(tmp, '00010000_00000005.dll')
            pair(supplemental, 2, 0x100,
                 provenance=MOD.HOSTED_MANIFEST_PROVENANCE)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 7

            # Legacy/no-P pairs remain unique.
            legacy_a = os.path.join(tmp, '00010000_00000006.dll')
            legacy_b = os.path.join(tmp, '00010000_00000007.dll')
            pair(legacy_a, 1, None, LOAD + 0x80)
            pair(legacy_b, 1, None, LOAD + 0x80)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 9

            # Directory enumeration and caller tier order cannot change the
            # inventory or its deterministic representative ordering.
            forward = MOD.cache_candidate_inventory([tmp, tmp], None)
            reverse = MOD.cache_candidate_inventory([tmp], None)
            assert forward[0] == reverse[0] == 9
            assert list(forward[1]) == list(reverse[1]) == sorted(forward[1])

        with tempfile.TemporaryDirectory() as tmp:
            invalid = os.path.join(tmp, '00010000_00000001.dll')
            pair(invalid, 2, 0x180)
            MOD._dll_runtime_exports_match = (
                lambda _path, _abi, _pair, _entries: False)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 0 and counts[invalid] == 0
            assert counts.raw_total == 2 and counts.file_total == 1
            assert counts.range_link_total == 2
            MOD._dll_runtime_exports_match = (
                lambda _path, _abi, _pair, entries: bool(entries))

        with tempfile.TemporaryDirectory() as tmp:
            # Repeated near-cap projections must not map and validate every
            # unchanged DLL again. File identity, manifest identity, expected
            # ABI, and validation callback changes each invalidate only the
            # affected cached result.
            calls = {'abi': 0, 'exports': 0}

            def counted_abi(_path, _abi):
                calls['abi'] += 1
                return True

            def counted_exports(_path, _abi, _pair, entries):
                calls['exports'] += 1
                return bool(entries)

            MOD._dll_abi_matches = counted_abi
            MOD._dll_runtime_exports_match = counted_exports
            cached = os.path.join(tmp, '00010000_00000001.dll')
            pair(cached, 2, 0x190)
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 2
            assert calls == {'abi': 1, 'exports': 1}
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 2
            assert calls == {'abi': 1, 'exports': 1}

            manifest_path = pathlib.Path(cached).with_suffix('.ranges')
            manifest_path.write_text(MOD.overlay_ranges_text(
                funcs(2), 0x190, MOD.HOSTED_MANIFEST_PROVENANCE))
            manifest_stat = manifest_path.stat()
            os.utime(manifest_path, ns=(manifest_stat.st_atime_ns,
                                       manifest_stat.st_mtime_ns + 1000000000))
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 2
            assert calls == {'abi': 1, 'exports': 2}

            dll_stat = os.stat(cached)
            os.utime(cached, ns=(dll_stat.st_atime_ns,
                                 dll_stat.st_mtime_ns + 1000000000))
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 2
            assert calls == {'abi': 2, 'exports': 3}

            # Atomic replacement must invalidate even when an adversarial
            # publisher preserves the old sizes and mtimes: the file identity
            # (and on Windows change time) still changes.
            replacement = os.path.join(tmp, '.replacement.dll')
            pair(replacement, 1, 0x192, LOAD + 0x100)
            replacement_ranges = pathlib.Path(replacement).with_suffix(
                '.ranges')
            old_dll_stat = os.stat(cached)
            old_ranges_stat = manifest_path.stat()
            os.utime(replacement, ns=(old_dll_stat.st_atime_ns,
                                      old_dll_stat.st_mtime_ns))
            os.utime(replacement_ranges,
                     ns=(old_ranges_stat.st_atime_ns,
                         old_ranges_stat.st_mtime_ns))
            os.replace(replacement, cached)
            os.replace(replacement_ranges, manifest_path)
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 1
            assert calls == {'abi': 3, 'exports': 4}
            assert MOD.cache_candidate_inventory([tmp], 15)[0] == 1
            assert calls == {'abi': 4, 'exports': 5}
            MOD._dll_abi_matches = original_abi
            MOD._dll_runtime_exports_match = (
                lambda _path, _abi, _pair, entries: bool(entries))

        with tempfile.TemporaryDirectory() as tmp:
            # A boolean validation failure may be a transient loader error, so
            # neither ABI nor full-export negative results may be memoized.
            calls = {'abi': 0, 'exports': 0}

            def transient_abi(_path, _abi):
                calls['abi'] += 1
                return calls['abi'] > 1

            def transient_exports(_path, _abi, _pair, entries):
                calls['exports'] += 1
                return bool(entries) and calls['exports'] > 1

            transient = os.path.join(tmp, '00010000_00000001.dll')
            pair(transient, 2, 0x191)
            MOD._dll_abi_matches = transient_abi
            MOD._dll_runtime_exports_match = transient_exports
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 0
            assert calls == {'abi': 1, 'exports': 0}
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 0
            assert calls == {'abi': 2, 'exports': 1}
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 2
            assert calls == {'abi': 2, 'exports': 2}
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 2
            assert calls == {'abi': 2, 'exports': 2}
            MOD._dll_abi_matches = original_abi
            MOD._dll_runtime_exports_match = (
                lambda _path, _abi, _pair, entries: bool(entries))

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            second = os.path.join(tmp, '00010000_00000002.dll')
            third = os.path.join(tmp, '00010000_00000003.dll')
            pair(first, 2, 0x200)
            pair(second, 2, 0x200)
            pair(third, 2, 0x200)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 2 and counts.raw_total == 6

            replacement = os.path.join(tmp, '.replacement.dll')
            pair(replacement, 1, 0x201, LOAD + 0x40)
            usage = MOD.projected_cache_candidate_usage(
                total, counts, [tmp], first, 1,
                staged_identity(replacement))
            # Replacing one of three references keeps the old group's cost.
            assert usage == (3, 5, 3)
            same_usage = MOD.projected_cache_candidate_usage(
                total, counts, [tmp], first, 2, staged_identity(second))
            assert same_usage == (2, 6, 3)

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            duplicate = os.path.join(tmp, '00010000_00000002.dll')
            overlapping = [(
                LOAD + 0x100, 0x500,
                ((LOAD, 0x900), (LOAD + 0x800, 0x900)))
            ]
            pair_records(first, overlapping, 0x280)
            pair_records(duplicate, overlapping, 0x280)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            # Each physical F touches pages 0 and 1. The overlapping page 0 is
            # linked once per F, while the duplicate physical pair costs again.
            assert total == 1 and counts.raw_total == 2
            assert counts.range_link_counts == {first: 2, duplicate: 2}
            assert counts.range_link_total == 4

            replacement = os.path.join(tmp, '.range-replacement.dll')
            replacement_records = [(
                LOAD + 0x100, 0x501, ((LOAD + 0x100, 4),))]
            pair_records(replacement, replacement_records, 0x281)
            replacement_links = MOD._runtime_manifest_range_link_count(
                replacement_records)
            usage = MOD._projected_cache_capacity_usage(
                total, counts, [tmp], first, 1,
                staged_identity(replacement), replacement_links)
            assert usage == (2, 2, 3, 2)

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            wide = [(LOAD, 0x600, ((LOAD, 9 * 4096),))]
            pair_records(first, wide, 0x290)
            staged = os.path.join(tmp, '.range-overflow.dll')
            pair_records(staged, wide, 0x290)
            try:
                MOD.publish_shard_pair(
                    staged, pathlib.Path(staged).with_suffix('.ranges'),
                    os.path.join(tmp, '00010000_00000002.dll'),
                    candidate_cap=1)
            except MOD.ShardCandidateCapacityError as exc:
                assert 'lazy range-link capacity' in str(exc)
            else:
                raise AssertionError('lazy range-link cap was not enforced')

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            second = os.path.join(tmp, '00010000_00000002.dll')
            wide = [(LOAD, 0x700, ((LOAD, 17 * 4096),))]
            pair_records(first, wide, 0x2A0)
            pair_records(second, wide, 0x2A0)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 1 and counts.range_link_total == 34
            assert MOD.cache_candidate_capacity_full(first, None, 2) == \
                (True, 1)
            assert MOD.candidate_capacity_saturated_in_tier(
                total, counts, 2, tmp)
            shrinking = os.path.join(tmp, '.range-shrink.dll')
            pair_records(shrinking, [(
                LOAD, 0x701, ((LOAD, 4),))], 0x2A1)
            # The namespace starts over its 32-link bound. A replacement that
            # reduces physical range-link use remains a legal repair.
            assert MOD.publish_shard_pair(
                shrinking, pathlib.Path(shrinking).with_suffix('.ranges'),
                first, candidate_cap=2)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 2 and counts.range_link_total == 18

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            second = os.path.join(tmp, '00010000_00000002.dll')
            # Deliberately repeat F while changing P: no pair dedup is legal.
            pair(first, 1, 1)
            pair(second, 1, 2)
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 2 and counts[first] == counts[second] == 1

            staged = os.path.join(tmp, '.staged.dll')
            staged_ranges = os.path.join(tmp, '.staged.ranges')
            pair(staged, 2, 3, LOAD + 0x20)
            third = os.path.join(tmp, '00010000_00000003.dll')
            assert MOD.publish_shard_pair(
                staged, staged_ranges, third, candidate_cap=4)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 4
            assert MOD.cache_candidate_capacity_full(third, None, 4) == \
                (True, 4)
            total, counts = MOD.cache_candidate_capacity_snapshot(third, None)
            assert MOD.candidate_capacity_saturated_in_tier(
                total, counts, 4, tmp)
            assert not MOD.candidate_capacity_saturated_in_tier(
                total, counts, 5, tmp)
            cache_hit = os.path.join(tmp, '.cache-hit.dll')
            cache_hit_ranges = os.path.join(tmp, '.cache-hit.ranges')
            pair(cache_hit, 1, 30, LOAD + 0x200)
            assert not MOD.publish_shard_pair(
                cache_hit, cache_hit_ranges, third,
                preserve_existing=True, candidate_cap=4)

            rejected_stage = os.path.join(tmp, '.rejected.dll')
            rejected_ranges = os.path.join(tmp, '.rejected.ranges')
            pair(rejected_stage, 1, 4, LOAD + 0x40)
            fourth = os.path.join(tmp, '00010000_00000004.dll')
            snapshot = {path: pathlib.Path(path).read_bytes()
                        for path in (first, second, third)}
            try:
                MOD.publish_shard_pair(
                    rejected_stage, rejected_ranges, fourth, candidate_cap=4)
            except MOD.ShardCandidateCapacityError:
                pass
            else:
                raise AssertionError('candidate boundary+1 publication accepted')
            assert not os.path.exists(fourth)
            assert snapshot == {path: pathlib.Path(path).read_bytes()
                                for path in snapshot}

            replacement = os.path.join(tmp, '.replacement.dll')
            replacement_ranges = os.path.join(tmp, '.replacement.ranges')
            pair(replacement, 1, 5, LOAD + 0x60)
            assert MOD.publish_shard_pair(
                replacement, replacement_ranges, third, candidate_cap=4)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 3
            assert MOD.cache_candidate_capacity_full(third, None, 4) == \
                (False, 3)
            old_dll = pathlib.Path(third).read_bytes()
            old_ranges = pathlib.Path(third).with_suffix('.ranges').read_text()
            growth = os.path.join(tmp, '.growth.dll')
            growth_ranges = os.path.join(tmp, '.growth.ranges')
            pair(growth, 3, 6, LOAD + 0x80)
            try:
                MOD.publish_shard_pair(
                    growth, growth_ranges, third, candidate_cap=4)
            except MOD.ShardCandidateCapacityError:
                pass
            else:
                raise AssertionError('over-cap force replacement accepted')
            assert pathlib.Path(third).read_bytes() == old_dll
            assert pathlib.Path(third).with_suffix('.ranges').read_text() == \
                old_ranges

        with tempfile.TemporaryDirectory() as tmp:
            # An already-impossible pair must be rejected from its generated
            # manifest before the expensive native linker runs. The normal
            # locked publication projection remains in place for admissions.
            existing = os.path.join(tmp, '00010000_00000001.dll')
            pair(existing, 2, 0x310)
            assert MOD.preflight_shard_candidate_capacity(
                existing, funcs(2, LOAD + 0x40), 0x312, None, None, 2
            ) is None
            final = os.path.join(tmp, '00010000_00000002.dll')
            attempted_link = []
            original_compile = MOD._compile_dll_direct

            def unexpected_link(*_args, **_kwargs):
                attempted_link.append(True)
                return False

            MOD._compile_dll_direct = unexpected_link
            try:
                publication = {}
                assert not MOD.compile_dll(
                    'unused.c', final, [], func_ids=funcs(1, LOAD + 0x80),
                    pair_id=0x311, publication_result=publication,
                    candidate_cap=2)
                assert not attempted_link
                assert 'capacity would be exceeded' in \
                    publication['capacity_error']
            finally:
                MOD._compile_dll_direct = original_compile

            # A stable impossible projection becomes a rejection-only witness;
            # repeated near-cap repairs do not enumerate every cache file.
            MOD._candidate_rejection_witness_cache.clear()
            original_inventory = MOD.cache_candidate_inventory
            inventory_calls = []

            def counted_inventory(*args, **kwargs):
                inventory_calls.append(True)
                return original_inventory(*args, **kwargs)

            MOD.cache_candidate_inventory = counted_inventory
            try:
                assert MOD.preflight_shard_candidate_capacity(
                    final, funcs(1, LOAD + 0x80), 0x313, None, None, 2)
                assert MOD.preflight_shard_candidate_capacity(
                    os.path.join(tmp, '00010000_00000003.dll'),
                    funcs(1, LOAD + 0xC0), 0x314, None, None, 2)
                assert len(inventory_calls) == 1
            finally:
                MOD.cache_candidate_inventory = original_inventory
                MOD._candidate_rejection_witness_cache.clear()

            # A stale witness that appears admissible is not authority: it must
            # force a fresh inventory before deciding whether to accept.
            capacity_lock, cache_dirs = MOD._candidate_capacity_namespace(final)
            cache_dirs_key = tuple(
                os.path.normcase(os.path.abspath(path))
                for path in cache_dirs)
            witness_key = (
                os.path.normcase(os.path.abspath(capacity_lock)),
                cache_dirs_key, None, 2, MOD._dll_abi_matches,
                MOD._dll_runtime_exports_match)
            MOD._candidate_rejection_witness_cache[witness_key] = (
                0, MOD.CandidateInventory())
            inventory_calls = []
            MOD.cache_candidate_inventory = counted_inventory
            try:
                assert MOD.preflight_shard_candidate_capacity(
                    final, funcs(1, LOAD + 0x80), 0x315, None, None, 2)
                assert len(inventory_calls) == 1
            finally:
                MOD.cache_candidate_inventory = original_inventory
                MOD._candidate_rejection_witness_cache.clear()

        with tempfile.TemporaryDirectory() as tmp:
            # Growth after an admitted preflight is caught by the unchanged
            # authoritative projection after linking.
            existing = os.path.join(tmp, '00010000_00000001.dll')
            racer = os.path.join(tmp, '00010000_00000003.dll')
            final = os.path.join(tmp, '00010000_00000002.dll')
            pair(existing, 1, 0x320)
            original_compile = MOD._compile_dll_direct

            def racing_link(_source, staged, *_args, **_kwargs):
                pathlib.Path(staged).write_bytes(b'DLL')
                pair(racer, 1, 0x321, LOAD + 0x40)
                return True

            MOD._compile_dll_direct = racing_link
            try:
                publication = {}
                assert not MOD.compile_dll(
                    'unused.c', final, [], func_ids=funcs(1, LOAD + 0x80),
                    pair_id=0x322, publication_result=publication,
                    candidate_cap=2)
                assert 'capacity would be exceeded' in \
                    publication['capacity_error']
                assert not os.path.exists(final)
            finally:
                MOD._compile_dll_direct = original_compile

        with tempfile.TemporaryDirectory() as tmp:
            hidden = os.path.join(tmp, '00010000_00000001.dll')
            other = os.path.join(tmp, '00010000_00000002.dll')
            pair(other, 1, 40)
            token = 'a' * 32
            backup_dll = f'{hidden}.pair-txn.{token}.old-dll'
            backup_ranges = f'{hidden}.pair-txn.{token}.old-ranges'
            pair(backup_dll, 1, 41, LOAD + 0x20)
            hidden_ranges = os.path.splitext(hidden)[0] + '.ranges'
            backup_manifest = pathlib.Path(backup_dll).with_suffix(
                '.ranges')
            # pair() names the backup manifest by with_suffix; move it to the
            # exact transaction backup path used by recovery.
            pathlib.Path(backup_ranges).write_text(backup_manifest.read_text())
            backup_manifest.unlink()
            MOD._write_json_atomic(hidden + '.pair-txn.json', {
                'schema': 'psxrecomp shard pair transaction v1',
                'token': token,
                'backup_dll': backup_dll,
                'backup_ranges': backup_ranges,
                'old_dll_sha256': MOD._file_sha256(backup_dll),
                'old_ranges_sha256': MOD._file_sha256(backup_ranges),
                'new_dll_sha256': '0' * 64,
                'new_ranges_sha256': '1' * 64,
            })
            staged = os.path.join(tmp, '.new.dll')
            staged_ranges = os.path.join(tmp, '.new.ranges')
            pair(staged, 1, 42, LOAD + 0x40)
            new_final = os.path.join(tmp, '00010000_00000003.dll')
            try:
                MOD.publish_shard_pair(
                    staged, staged_ranges, new_final, candidate_cap=2)
            except MOD.ShardCandidateCapacityError:
                pass
            else:
                raise AssertionError('transaction-hidden pair was not counted')
            assert os.path.exists(hidden) and os.path.exists(hidden_ranges)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 2
            assert not os.path.exists(new_final)

        with tempfile.TemporaryDirectory() as tmp:
            game = pathlib.Path(tmp) / 'SCUS-TEST'
            gcc_dir = game / 'gcc' / 'win-x64' / 'cg5_7125d9b5'
            tcc_dir = game / 'tcc' / 'win-x64' / 'cg5_7125d9b5'
            gcc_dir.mkdir(parents=True)
            tcc_dir.mkdir(parents=True)
            basename = '00010000_00000001.dll'
            gcc_pair = str(gcc_dir / basename)
            tcc_pair = str(tcc_dir / basename)
            pair(gcc_pair, 2, 10)
            pair(tcc_pair, 3, 11)
            tier_dirs = [str(gcc_dir), str(tcc_dir)]
            total, counts = MOD.cache_candidate_inventory(tier_dirs, None)
            assert total == 2 and counts[gcc_pair] == 2
            assert tcc_pair not in counts
            assert MOD.candidate_capacity_saturated_in_tier(
                total, counts, 2, str(gcc_dir))
            gcc_exact = str(gcc_dir / '00010000_00000002.dll')
            tcc_exact = str(tcc_dir / '00010000_00000003.dll')
            pair(gcc_exact, 1, 15, LOAD + 0x40)
            pair(tcc_exact, 1, 15, LOAD + 0x40)
            total, counts = MOD.cache_candidate_inventory(tier_dirs, None)
            assert total == 4 and counts[gcc_exact] == counts[tcc_exact] == 1
            for exact in (gcc_exact, tcc_exact):
                pathlib.Path(exact).unlink()
                pathlib.Path(exact).with_suffix('.ranges').unlink()
            lower_unique = str(tcc_dir / '00010000_00000002.dll')
            pair(lower_unique, 1, 14, LOAD + 0x80)
            total, counts = MOD.cache_candidate_inventory(tier_dirs, None)
            assert total == 3 and counts[lower_unique] == 1
            assert not MOD.candidate_capacity_saturated_in_tier(
                total, counts, 3, str(gcc_dir))
            pathlib.Path(lower_unique).unlink()
            pathlib.Path(lower_unique).with_suffix('.ranges').unlink()
            total, counts = MOD.cache_candidate_inventory(tier_dirs, None)
            assert MOD.projected_cache_candidate_count(
                total, counts, tier_dirs, tcc_pair, 4) == 2
            assert MOD.projected_cache_candidate_count(
                total, counts, tier_dirs, gcc_pair, 1) == 1
            MOD._dll_abi_matches = (
                lambda path, _abi: os.path.normcase(path) !=
                os.path.normcase(gcc_pair))
            total, counts = MOD.cache_candidate_inventory(tier_dirs, 14)
            assert total == 3 and counts[tcc_pair] == 3
            assert gcc_pair not in counts
            MOD._dll_abi_matches = original_abi

        with tempfile.TemporaryDirectory() as tmp:
            game = pathlib.Path(tmp) / 'SCUS-CASE'
            gcc_dir = game / 'gcc' / 'win-x64' / 'cg5_06162507'
            tcc_dir = game / 'tcc' / 'win-x64' / 'cg5_06162507'
            gcc_dir.mkdir(parents=True)
            tcc_dir.mkdir(parents=True)
            upper = str(gcc_dir / '00010000_DEADBEEF.dll')
            lower = str(tcc_dir / '00010000_deadbeef.dll')
            pair(upper, 2, 12)
            pair(lower, 3, 13)
            total, counts = MOD.cache_candidate_inventory(
                [str(gcc_dir), str(tcc_dir)], None)
            if os.name == 'nt':
                assert total == 2 and counts == {upper: 2}
            else:
                assert total == 5 and counts == {upper: 2, lower: 3}

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            second = os.path.join(tmp, '00010000_00000002.dll')
            pair(first, 3, 20)
            pair(second, 2, 21, LOAD + 0x100)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 5
            shrinking = os.path.join(tmp, '.shrinking.dll')
            shrinking_ranges = os.path.join(tmp, '.shrinking.ranges')
            pair(shrinking, 1, 22, LOAD + 0x200)
            # Still over cap after replacement (5 -> 3), but the operation is
            # non-growing and therefore improves the process-lifetime bound.
            assert MOD.publish_shard_pair(
                shrinking, shrinking_ranges, first, candidate_cap=2)
            assert MOD.cache_candidate_inventory([tmp], None)[0] == 3

        with tempfile.TemporaryDirectory() as tmp:
            first = os.path.join(tmp, '00010000_00000001.dll')
            pair(first, 2, 0x300)
            for suffix in (2, 3):
                staged = os.path.join(tmp, f'.raw-{suffix}.dll')
                pair(staged, 2, 0x300)
                final = os.path.join(tmp, f'00010000_{suffix:08X}.dll')
                if suffix == 2:
                    assert MOD.publish_shard_pair(
                        staged, pathlib.Path(staged).with_suffix('.ranges'),
                        final, candidate_cap=2)
                else:
                    try:
                        MOD.publish_shard_pair(
                            staged, pathlib.Path(staged).with_suffix('.ranges'),
                            final, candidate_cap=2)
                    except MOD.ShardCandidateCapacityError as exc:
                        assert 'lazy-manifest capacity' in str(exc)
                    else:
                        raise AssertionError('raw lazy F cap was not enforced')
            total, counts = MOD.cache_candidate_inventory([tmp], None)
            assert total == 2 and counts.raw_total == 4
            assert MOD.candidate_capacity_saturated_in_tier(
                total, counts, 2, tmp)

        with tempfile.TemporaryDirectory() as tmp:
            original_file_cap = MOD.RUNTIME_CACHE_FILE_CAP
            MOD.RUNTIME_CACHE_FILE_CAP = 2
            try:
                for suffix in (1, 2):
                    final = os.path.join(tmp, f'00010000_{suffix:08X}.dll')
                    pair(final, 1, 0x400 + suffix, LOAD + suffix * 4)
                staged = os.path.join(tmp, '.file-cap.dll')
                pair(staged, 1, 0x403, LOAD + 0x20)
                try:
                    MOD.publish_shard_pair(
                        staged, pathlib.Path(staged).with_suffix('.ranges'),
                        os.path.join(tmp, '00010000_00000003.dll'),
                        candidate_cap=10)
                except MOD.ShardCandidateCapacityError as exc:
                    assert 'cache file capacity' in str(exc)
                else:
                    raise AssertionError('selected physical file cap not enforced')
                total, counts = MOD.cache_candidate_inventory([tmp], None)
                assert MOD.candidate_capacity_saturated_in_tier(
                    total, counts, 10, tmp)
            finally:
                MOD.RUNTIME_CACHE_FILE_CAP = original_file_cap
    finally:
        MOD._dll_runtime_exports_match = original_exports
        MOD._dll_abi_matches = original_abi

    if os.name == 'nt':
        # Exact pairs racing to distinct names may both commit: the namespace
        # lock makes the second writer observe the first pair's dedup identity.
        gcc = r'C:\msys64\mingw64\bin\gcc.exe'
        assert os.path.isfile(gcc)
        with tempfile.TemporaryDirectory() as tmp:
            pair_id = 0x1020304050607080
            source = pathlib.Path(tmp) / 'capacity.c'
            template = pathlib.Path(tmp) / 'template.dll'
            source.write_text(
                '#include <stdint.h>\n'
                '__declspec(dllexport) int overlay_abi(void){return 14;}\n'
                '__declspec(dllexport) uint64_t overlay_pair_id(void){'
                f'return UINT64_C(0x{pair_id:016X});' '}\n'
                '__declspec(dllexport) void overlay_init(const void*p){(void)p;}\n'
                '__declspec(dllexport) void overlay_flush_cycles(void){}\n'
                '__declspec(dllexport) void func_80010000(void*p){(void)p;}\n')
            subprocess.run(
                [gcc, '-shared', str(source), '-o', str(template)],
                check=True, capture_output=True, text=True)
            partial_manifest = MOD.overlay_ranges_text([
                (LOAD, 1, ((LOAD, 4),)),
                (LOAD + 4, 2, ((LOAD + 4, 4),)),
            ], pair_id)
            template.with_suffix('.ranges').write_text(partial_manifest)
            assert MOD.load_shard_func_ids(str(template), 14) == []
            template.with_suffix('.ranges').unlink()
            manifest = MOD.overlay_ranges_text(
                [(LOAD, 1, ((LOAD, 4),))], pair_id)
            module_path = str(ROOT / 'tools' / 'compile_overlays.py')
            child_script = r'''
import importlib.util, sys
s=importlib.util.spec_from_file_location("co",sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
try:
    m.publish_shard_pair(sys.argv[2],sys.argv[3],sys.argv[4],expected_existing_abi=14,candidate_cap=1)
except m.ShardCandidateCapacityError:
    raise SystemExit(23)
'''
            writers = []
            for index in range(2):
                staged = pathlib.Path(tmp) / f'.race-{index}.dll'
                staged_ranges = pathlib.Path(tmp) / f'.race-{index}.ranges'
                shutil.copy2(template, staged)
                staged_ranges.write_text(manifest)
                final = pathlib.Path(tmp) / f'00010000_{index + 1:08X}.dll'
                writers.append(subprocess.Popen([
                    sys.executable, '-c', child_script, module_path,
                    str(staged), str(staged_ranges), str(final)]))
            codes = sorted(writer.wait(timeout=30) for writer in writers)
            assert codes == [0, 0], codes
            total, counts = MOD.cache_candidate_inventory([tmp], 14)
            assert total == 1 and counts.raw_total == 2

        # Distinct pairs still compete at cap-1; exactly one may commit.
        with tempfile.TemporaryDirectory() as tmp:
            module_path = str(ROOT / 'tools' / 'compile_overlays.py')
            child_script = r'''
import importlib.util, sys
s=importlib.util.spec_from_file_location("co",sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
try:
    m.publish_shard_pair(sys.argv[2],sys.argv[3],sys.argv[4],expected_existing_abi=14,candidate_cap=1)
except m.ShardCandidateCapacityError:
    raise SystemExit(23)
'''
            writers = []
            for index, pair_id in enumerate(
                    (0x1020304050607080, 0x1020304050607081)):
                source = pathlib.Path(tmp) / f'distinct-{index}.c'
                template = pathlib.Path(tmp) / f'.distinct-{index}.dll'
                source.write_text(
                    '#include <stdint.h>\n'
                    '__declspec(dllexport) int overlay_abi(void){return 14;}\n'
                    '__declspec(dllexport) uint64_t overlay_pair_id(void){'
                    f'return UINT64_C(0x{pair_id:016X});' '}\n'
                    '__declspec(dllexport) void overlay_init(const void*p){(void)p;}\n'
                    '__declspec(dllexport) void overlay_flush_cycles(void){}\n'
                    '__declspec(dllexport) void func_80010000(void*p){(void)p;}\n')
                subprocess.run(
                    [gcc, '-shared', str(source), '-o', str(template)],
                    check=True, capture_output=True, text=True)
                staged_ranges = pathlib.Path(tmp) / f'.distinct-{index}.ranges'
                staged_ranges.write_text(MOD.overlay_ranges_text(
                    [(LOAD, 1, ((LOAD, 4),))], pair_id))
                final = pathlib.Path(tmp) / f'00010000_{index + 1:08X}.dll'
                writers.append(subprocess.Popen([
                    sys.executable, '-c', child_script, module_path,
                    str(template), str(staged_ranges), str(final)]))
            codes = sorted(writer.wait(timeout=30) for writer in writers)
            assert codes == [0, 23], codes
            assert MOD.cache_candidate_inventory([tmp], 14)[0] == 1


def check_interior_fragment_contract():
    entry = LOAD + 0x20
    assert MOD.walk_root_seed_entries([
        f"producer_range 0x{LOAD:08X} 0x{LOAD + 0x100:08X}",
        f"cross_call_allow 0x{LOAD + 0x200:08X}",
        f"interior 0x{entry:08X}",
        f"call_root 0x{LOAD:08X}",
        f"dispatch_root 0x{LOAD + 4:08X}",
        f"0x{LOAD + 8:08X}",
    ]) == {LOAD, LOAD + 4, LOAD + 8}
    good = [(entry, 0x12345678, [(entry, 8)])]
    assert MOD.validate_interior_fragment_ids(entry, good, {entry}) is None
    second = entry + 0x40
    multi = good + [(second, 0x87654321, [(second, 12)])]
    assert MOD.validate_fragment_requested_ids(
        {entry, second}, multi, {entry, second}) is None

    host = LOAD
    hosted_target = LOAD + 0x20
    owner = (host, 0xAABBCCDD, [(host, 0x40)])
    hosted_spec = {
        'host': host,
        'owner_identity': (host, owner[1], ((host, 0x40),)),
        'host_reason': 'STATIC_DISCOVERY_ROOT',
    }
    hosted_good = [
        owner,
        (hosted_target, owner[1], owner[2]),
    ]
    # Only the true single-range root may nominate a sibling target. Hosted
    # aliases and a later rooted island in a multi-range identity stay inert.
    assert MOD.organic_root_entries(hosted_good) == {host}
    assert MOD.organic_root_entries([
        (hosted_target, owner[1], owner[2]),
        (hosted_target, owner[1], [(host, 8), (hosted_target, 8)]),
        (hosted_target, owner[1], [(hosted_target, 0)]),
    ]) == set()
    hosted_specs = {hosted_target: hosted_spec}
    assert MOD.validate_hosted_fragment_ids(
        {hosted_target}, hosted_specs, hosted_good,
        {host, hosted_target}) is None
    assert 'changed selected owner' in MOD.validate_hosted_fragment_ids(
        {hosted_target}, hosted_specs,
        [(host, owner[1] ^ 1, owner[2]), hosted_good[1]],
        {host, hosted_target})
    assert 'changed selected owner' in MOD.validate_hosted_fragment_ids(
        {hosted_target}, hosted_specs,
        [owner, (hosted_target, owner[1], [(host, 0x44)])],
        {host, hosted_target})
    host2 = LOAD + 0x80
    target2 = host2 + 0x20
    owner2 = (host2, 0x11223344, [(host2, 0x40)])
    spec2 = {
        'host': host2, 'owner_identity': owner2,
        'host_reason': 'STATIC_DISPATCH_ENTRY',
    }
    assert MOD.validate_hosted_fragment_ids(
        {hosted_target, target2},
        {hosted_target: hosted_spec, target2: spec2},
        hosted_good + [owner2, (target2, owner2[1], owner2[2])],
        {host, hosted_target, host2, target2}) is None
    assert 'target/spec set mismatch' in MOD.validate_hosted_fragment_ids(
        {hosted_target, target2}, {hosted_target: hosted_spec},
        hosted_good, {host, hosted_target})
    assert MOD.validate_hosted_fragment_ids(
        {hosted_target}, hosted_specs, [owner], {host},
        allow_missing_targets=True) is None
    assert MOD.hosted_fragment_served_targets(
        {hosted_target}, [owner], {host}) == set()
    assert "requested entry" in MOD.validate_fragment_requested_ids(
        {entry, second + 4}, multi, {entry, second})
    assert "missing from generated C" in MOD.validate_interior_fragment_ids(
        entry, good, set())
    assert "0 manifest identities" in MOD.validate_interior_fragment_ids(
        entry, [], {entry})
    assert "2 manifest identities" in MOD.validate_interior_fragment_ids(
        entry, good + good, {entry})
    assert "0 ranges" in MOD.validate_interior_fragment_ids(
        entry, [(entry, 0, [])], {entry})
    assert "17 ranges" in MOD.validate_interior_fragment_ids(
        entry, [(entry, 0, [(entry + i * 4, 4) for i in range(17)])],
        {entry})
    assert "outside its guarded ranges" in MOD.validate_interior_fragment_ids(
        entry, [(entry, 0, [(entry + 4, 4)])], {entry})
    assert "fragment entry" in MOD.validate_interior_fragment_ids(
        entry, good + [(entry + 0x40, 0,
                        [(entry + 0x40 + i * 4, 4) for i in range(17)])],
        {entry, entry + 0x40})
    assert "empty guarded range" in MOD.validate_interior_fragment_ids(
        entry, good + [(entry + 0x40, 0, [(entry + 0x40, 0)])],
        {entry, entry + 0x40})
    assert "region entry" in MOD.validate_overlay_func_ids(
        [(entry, 0, [(entry + i * 4, 4) for i in range(17)])],
        "region")
    assert MOD.interior_failure_is_deterministic(
        "generated-c-audit: unsupported")
    assert MOD.interior_failure_is_deterministic(
        "requested-entry-audit: omitted")
    assert not MOD.interior_failure_is_deterministic(
        "fragment cache-key collision/stale pair")
    assert not MOD.interior_failure_is_deterministic(
        "candidate-capacity: full")

    static_only_job = {
        "static_demands": {entry}, "executed": set(), "forced": set()}
    assert MOD.optional_static_fragment_rejection(
        entry, static_only_job, "generated-c-audit: unsupported")
    assert not MOD.optional_static_fragment_rejection(
        entry, dict(static_only_job, executed={entry}),
        "generated-c-audit: unsupported")
    assert not MOD.optional_static_fragment_rejection(
        entry, dict(static_only_job, forced={entry}),
        "requested-entry-audit: omitted")
    assert not MOD.optional_static_fragment_rejection(
        entry, static_only_job, "compile-error")
    assert MOD.interior_fail_memo_action(
        entry, static_only_job, False) == "skip"
    assert MOD.interior_fail_memo_action(
        entry, dict(static_only_job, executed={entry}), False) == "fail"
    assert MOD.interior_fail_memo_action(
        entry, dict(static_only_job, forced={entry}), False) == "fail"
    assert MOD.interior_fail_memo_action(
        entry, dict(static_only_job, executed={entry}), True) == "retry"
    memo_key = MOD._interior_fail_key(
        LOAD & 0x1FFFFFFF, entry, bytes(0x100), LOAD, 0x100,
        ((LOAD, LOAD + 0x100),), (), 14, False, {"game": {}})
    assert memo_key != MOD._interior_fail_key(
        LOAD & 0x1FFFFFFF, entry, bytes(0x100), LOAD, 0x100,
        ((LOAD, LOAD + 0x80),), (), 14, False, {"game": {}})
    assert memo_key != MOD._interior_fail_key(
        LOAD & 0x1FFFFFFF, entry, bytes(0x100), LOAD, 0x100,
        ((LOAD, LOAD + 0x100),), (), 14 | (1 << 16), False,
        {"game": {}})

    audit = {
        "included_reasons": {},
        "executed_pcs": set(),
        "static_exact_fragment_demands": set(),
        "static_interval_fragment_demands": set(),
        "producer_ranges": [(LOAD, LOAD + 0x100)],
        "accepted_cross_producer_calls": {LOAD + 0x200},
    }
    data = bytes(0x100)
    assert MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit, set()) is None
    job = MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit, {entry})
    assert job is not None
    assert job["candidates"] == {entry}
    assert job["forced"] == {entry}
    assert job["executed"] == set()
    assert job["static_demands"] == set()
    assert job["static_exact_demands"] == set()
    assert job["static_interval_demands"] == set()
    assert job["producer_ranges"] == ((LOAD, LOAD + 0x100),)
    assert job["cross_call_allow"] == (LOAD + 0x200,)
    assert MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit,
        {LOAD + 0x200}) is None

    # Strong current-byte static provenance schedules the same isolated,
    # fail-closed fragment path without requiring a played capture.
    static_audit = dict(audit, static_exact_fragment_demands={entry})
    static_job = MOD.make_interior_fragment_job(
        LOAD & 0x1FFFFFFF, LOAD, len(data), data, static_audit, set())
    assert static_job is not None
    assert static_job["candidates"] == {entry}
    assert static_job["static_demands"] == {entry}
    assert static_job["static_exact_demands"] == {entry}
    assert static_job["static_interval_demands"] == set()
    assert static_job["executed"] == set()
    assert static_job["forced"] == set()

    recipient = dict(static_job)
    recipient['included_reasons'] = {
        host: {'STATIC_DISCOVERY_ROOT'},
    }
    recipient['static_exact_demands'] = {host}
    recipient['producer_ranges'] = ((LOAD, LOAD + 0x100),)
    donor = dict(recipient)
    donor['hosted_donor_demands'] = {hosted_target}
    donor_recipes = {hosted_target: [donor]}
    selected = MOD.select_hosted_interior_demands(
        recipient, donor_recipes, [owner, owner])
    assert selected == {hosted_target: hosted_spec}, selected
    local_root_failed = dict(
        recipient, static_exact_demands={host, hosted_target})
    assert MOD.select_hosted_interior_demands(
        local_root_failed, donor_recipes, [owner]) == selected
    # Alias-only geometry cannot become an owner, nor can an ambiguous overlap.
    assert MOD.select_hosted_interior_demands(
        recipient, donor_recipes,
        [(host, owner[1], [(host + 4, 0x3C)])]) == {}
    unrelated_overlap = dict(recipient)
    unrelated_overlap['included_reasons'] = {
        host: {'STATIC_DISCOVERY_ROOT'},
        host + 8: {'DISPATCH_ENTRY'},
    }
    assert MOD.select_hosted_interior_demands(
        unrelated_overlap, donor_recipes,
        [owner, (host + 8, 0x11223344, [(host + 8, 0x30)])]) == {}
    # Explicit producer partitions prevent a sibling address from attaching to
    # a host owned by different loaded bytes.
    split_recipient = dict(recipient)
    split_recipient['producer_ranges'] = (
        (LOAD, LOAD + 0x20), (LOAD + 0x20, LOAD + 0x100))
    assert MOD.select_hosted_interior_demands(
        split_recipient, donor_recipes, [owner]) == {}
    changed_donor = dict(donor, data=b'\x01' + donor['data'][1:])
    # Sibling bytes only nominate the address. Recipient ownership and emitted
    # identity provide the executable proof, so variant differences are valid.
    assert MOD.select_hosted_interior_demands(
        recipient, {hosted_target: [changed_donor]}, [owner]) == selected

    # Apply the recipient cap only after discarding its own donor evidence and
    # entries that are already native. Neither may displace a later useful
    # sibling proof.
    proofs, truncated = MOD.recipient_sibling_proofs(
        {0: (hosted_target,), 1: (hosted_target + 4,)},
        recipient_index=0, authority_phys_entries=set(), limit=1)
    assert proofs == {hosted_target + 4: True} and not truncated
    proofs, truncated = MOD.recipient_sibling_proofs(
        {0: (), 1: (hosted_target, hosted_target + 4,
                     hosted_target + 8)},
        recipient_index=0,
        authority_phys_entries={hosted_target & 0x1FFFFFFF}, limit=1)
    assert proofs == {hosted_target + 4: True} and truncated
    # Supplemental coverage cannot turn any bounded selection stage into
    # cross-invocation pagination. Keep the fixed proof window independent of
    # supplemental coverage; still_needed filters only the already-selected
    # work at compile time.
    proofs, truncated = MOD.recipient_sibling_proofs(
        {0: (), 1: (hosted_target, hosted_target + 4)},
        recipient_index=0, authority_phys_entries=set(), limit=1)
    assert proofs == {hosted_target: True} and truncated

    # Optional evidence caps are deterministic safe drops, never uncaught
    # whole-pipeline failures. Address order defines the retained subset.
    old_nomination_cap = MOD.HOSTED_NOMINATION_CAP
    old_host_cap = MOD.HOSTED_SELECTED_HOST_CAP
    try:
        MOD.HOSTED_NOMINATION_CAP = 1
        cap_audit = {}
        capped = MOD.select_hosted_interior_demands(
            recipient,
            {hosted_target: [donor], hosted_target + 4: [donor]},
            [owner], cap_audit)
        assert capped == {hosted_target: hosted_spec}
        assert cap_audit['nomination_cap_dropped'] == 1

        MOD.HOSTED_NOMINATION_CAP = old_nomination_cap
        MOD.HOSTED_SELECTED_HOST_CAP = 1
        two_host_recipient = dict(recipient)
        two_host_recipient['included_reasons'] = {
            host: {'STATIC_DISCOVERY_ROOT'},
            host2: {'STATIC_DISPATCH_ENTRY'},
        }
        cap_audit = {}
        capped = MOD.select_hosted_interior_demands(
            two_host_recipient,
            {hosted_target: [donor], target2: [donor]},
            [owner, owner2], cap_audit)
        assert list(capped) == [hosted_target]
        assert cap_audit['selected_host_cap_dropped'] == 1

        # Fixed-point regression: once host 1 is published, its supplemental
        # target must still occupy the stable host-cap slot on run 2. Coverage
        # is consulted only by the compile-time still_needed predicate, so host
        # 2 cannot page into the selection and publish on an unchanged rerun.
        fixed_proofs, _ = MOD.recipient_sibling_proofs(
            {0: (), 1: (hosted_target, target2)},
            recipient_index=0, authority_phys_entries=set())
        run1_selected = MOD.select_hosted_interior_demands(
            two_host_recipient, fixed_proofs, [owner, owner2])
        assert list(run1_selected) == [hosted_target]
        run1_compiles = []
        assert MOD.compile_batched_hosted_aliases(
            run1_selected,
            lambda specs: (run1_compiles.append(sorted(specs)) or
                           ([owner], 'built')),
            lambda entries, _ids, _status: set(entries),
            lambda _entry, status: (_ for _ in ()).throw(
                AssertionError(status))) == 1
        assert run1_compiles == [[hosted_target]]

        run2_selected = MOD.select_hosted_interior_demands(
            two_host_recipient, fixed_proofs, [owner, owner2])
        assert list(run2_selected) == [hosted_target]
        run2_compiles = []
        assert MOD.compile_batched_hosted_aliases(
            run2_selected,
            lambda specs: (run2_compiles.append(sorted(specs)) or
                           ([owner], 'built')),
            lambda entries, _ids, _status: set(entries),
            lambda _entry, status: (_ for _ in ()).throw(
                AssertionError(status)),
            still_needed=lambda entry: entry != hosted_target) == 0
        assert run2_compiles == []
    finally:
        MOD.HOSTED_NOMINATION_CAP = old_nomination_cap
        MOD.HOSTED_SELECTED_HOST_CAP = old_host_cap

    # Multi-host batches split conflicting hosts before splitting the aliases
    # sharing one owner. One bad alias can then fail alone without discarding
    # its good sibling or another host group.
    hosted_map = {
        hosted_target: hosted_spec,
        hosted_target + 4: hosted_spec,
        target2: spec2,
    }
    compile_calls = []
    successes = []
    failures = []

    def fake_compile(specs):
        compile_calls.append(tuple(sorted(specs)))
        hosts = {spec['host'] for spec in specs.values()}
        if len(hosts) > 1:
            return None, 'hosted-entry-audit: overlapping roots'
        bad = hosted_target + 4
        if bad in specs:
            return None, 'hosted-entry-audit: bad block'
        return [(entry, 1, [(spec['host'], 4)])
                for entry, spec in specs.items()], 'built'

    shard_count = MOD.compile_batched_hosted_aliases(
        hosted_map, fake_compile,
        lambda entries, _ids, _status: successes.extend(entries),
        lambda entry, _status: failures.append(entry))
    assert shard_count == 2
    assert successes == [hosted_target, target2]
    assert failures == [hosted_target + 4]
    assert set(compile_calls[0]) == set(hosted_map)
    assert any(call == (hosted_target, hosted_target + 4)
               for call in compile_calls)

    # A successful hosted compile can legitimately emit only a subset of the
    # requested block leaders. Close that remainder now instead of relying on a
    # later cache-filtered invocation to shrink the batch.
    partial_calls = []
    partial_served = []

    def partial_compile(specs):
        entries = tuple(sorted(specs))
        partial_calls.append(entries)
        emitted = entries[0]
        return [(emitted, 1, [(specs[emitted]['host'], 4)])], 'built'

    def partial_success(_entries, ids, _status):
        served = {identity[0] for identity in ids}
        partial_served.extend(sorted(served))
        return served

    partial_map = {
        hosted_target + offset: hosted_spec for offset in (0, 4, 8)
    }
    assert MOD.compile_batched_hosted_aliases(
        partial_map, partial_compile, partial_success,
        lambda entry, reason: (_ for _ in ()).throw(
            AssertionError((entry, reason))), attempt_cap=5) == 3
    assert partial_calls == [
        (hosted_target, hosted_target + 4, hosted_target + 8),
        (hosted_target + 4, hosted_target + 8),
        (hosted_target + 8,),
    ]
    assert partial_served == [
        hosted_target, hosted_target + 4, hosted_target + 8]
    assert MOD.HOSTED_COMPILE_ATTEMPT_CAP >= \
        2 * MOD.HOSTED_SELECTED_ALIAS_CAP - 1

    partition_calls = []
    partition_failures = []

    def reject_every_batch(specs):
        partition_calls.append(tuple(sorted(specs)))
        return None, 'hosted-entry-audit: synthetic rejection'

    assert MOD.compile_batched_hosted_aliases(
        partial_map, reject_every_batch,
        lambda *_args: (_ for _ in ()).throw(AssertionError('success')),
        lambda entry, _reason: partition_failures.append(entry),
        should_bisect=MOD.fragment_batch_failure_is_partitionable,
        attempt_cap=5) == 0
    assert len(partition_calls) == 5
    assert sorted(partition_failures) == sorted(partial_map)

    # Saturation is global for the invocation: once one chunk reports a full
    # Candidate table, do not invoke the toolchain for later chunks.
    full_calls = []
    capacity_full = [False]
    many_targets = {
        hosted_target + 0x100 + index * 4: hosted_spec
        for index in range(MOD.HOSTED_BATCH_ALIAS_CAP + 1)
    }

    def full_compile(specs):
        full_calls.append(tuple(sorted(specs)))
        return None, 'candidate-capacity: full; synthetic'

    def full_batch_failure(_entries, status):
        assert status.startswith('candidate-capacity: full')
        capacity_full[0] = True

    assert MOD.compile_batched_hosted_aliases(
        many_targets, full_compile,
        lambda *_args: (_ for _ in ()).throw(AssertionError('success')),
        lambda *_args: (_ for _ in ()).throw(AssertionError('singleton')),
        should_bisect=MOD.fragment_batch_failure_is_partitionable,
        on_batch_failure=full_batch_failure,
        stop_requested=lambda: capacity_full[0]) == 0
    assert len(full_calls) == 1

    covered_target = hosted_target
    rejected_target = hosted_target + 4
    missing_target = hosted_target + 8
    assert MOD.hosted_entries_still_missing(
        {covered_target: hosted_spec, rejected_target: hosted_spec,
         missing_target: hosted_spec},
        {covered_target & 0x1FFFFFFF}, {rejected_target}) == [missing_target]

    forced = {missing_target}
    assert MOD.fragment_capacity_allows_entry(False, False, (), covered_target)
    assert MOD.fragment_capacity_allows_entry(True, True, (), covered_target)
    assert MOD.fragment_capacity_allows_entry(
        True, False, forced, missing_target)
    assert not MOD.fragment_capacity_allows_entry(
        True, False, forced, covered_target)
    assert MOD.fragment_capacity_fastpath_eligible(
        True, False, [{'forced': set()}])
    assert not MOD.fragment_capacity_fastpath_eligible(
        False, False, [{'forced': set()}])
    assert not MOD.fragment_capacity_fastpath_eligible(
        True, True, [{'forced': set()}])
    assert not MOD.fragment_capacity_fastpath_eligible(
        True, False, [{'forced': forced}])

    # Same-byte/full-recipe captures union their root evidence into one bounded
    # supplemental job; a byte or producer recipe change remains isolated.
    other = entry + 0x40
    same_recipe = dict(static_job)
    same_recipe["candidates"] = {other}
    same_recipe["static_demands"] = {other}
    same_recipe["static_exact_demands"] = {other}
    merged = MOD.merge_fragment_jobs_by_recipe([static_job, same_recipe])
    assert len(merged) == 1
    assert merged[0]["static_exact_demands"] == {entry, other}
    different_bytes = dict(same_recipe, data=b"x" * len(data))
    forward = MOD.merge_fragment_jobs_by_recipe(
        [static_job, different_bytes])
    reverse = MOD.merge_fragment_jobs_by_recipe(
        [different_bytes, static_job])
    assert len(forward) == 2
    assert [item['data'] for item in forward] == [
        item['data'] for item in reverse]
    resident_recipe = dict(
        same_recipe,
        resident_cap={"producer": "bios_resident_manifest",
                      "bios_sha256": "00" * 32})
    assert len(MOD.merge_fragment_jobs_by_recipe(
        [static_job, resident_recipe], (14, 0, False, "gcc", "toml"))) == 2

    calls = []
    successes = []
    failures = []

    def batch_ok(roots):
        calls.append(tuple(roots))
        return [(root, root, [(root, 4)]) for root in roots], "built"

    assert MOD.compile_batched_fragment_roots(
        {entry, other}, batch_ok,
        lambda roots, ids, status: successes.append(
            (tuple(roots), len(ids), status)),
        lambda root, status: failures.append((root, status))) == 1
    assert calls == [(entry, other)]
    assert successes == [((entry, other), 2, "built")]
    assert failures == []

    calls.clear()
    successes.clear()
    bad = other

    def batch_bisect(roots):
        calls.append(tuple(roots))
        if bad in roots:
            return None, "requested-entry-audit: rejected"
        return [(root, root, [(root, 4)]) for root in roots], "built"

    assert MOD.compile_batched_fragment_roots(
        {entry, other}, batch_bisect,
        lambda roots, ids, status: successes.append(tuple(roots)),
        lambda root, status: failures.append((root, status))) == 1
    assert calls == [(entry, other), (entry,), (other,)]
    assert successes == [(entry,)]
    assert failures[-1] == (other, "requested-entry-audit: rejected")

    # Re-filter after the successful left half: its reachable manifest may
    # already provide a right-side requested entry, so do not publish a
    # redundant second supplement.
    calls.clear()
    successes.clear()
    failures.clear()
    current_entries = set()

    def left_reaches_right(roots):
        calls.append(tuple(roots))
        if len(roots) > 1:
            return None, "requested-entry-audit: split"
        return [(entry, 1, [(entry, 4)]),
                (other, 2, [(other, 4)])], "built"

    def warm_from_success(roots, ids, _status):
        successes.append(tuple(roots))
        current_entries.update(ev & 0x1FFFFFFF for ev, _crc, _ranges in ids)

    assert MOD.compile_batched_fragment_roots(
        {entry, other}, left_reaches_right, warm_from_success,
        lambda root, status: failures.append((root, status)),
        lambda root: (root & 0x1FFFFFFF) not in current_entries) == 1
    assert calls == [(entry, other), (entry,)]
    assert successes == [(entry,)]
    assert failures == []

    calls.clear()
    batch_failures = []

    def toolchain_down(roots):
        calls.append(tuple(roots))
        return None, "compile-error (toolchain unavailable)"

    assert not MOD.fragment_batch_failure_is_partitionable(
        "compile-error (toolchain unavailable)")
    assert MOD.fragment_batch_failure_is_partitionable(
        "requested-entry-audit: rejected")
    assert not MOD.fragment_batch_failure_is_partitionable(
        "candidate-capacity: full")
    assert MOD.compile_batched_fragment_roots(
        {entry, other}, toolchain_down,
        lambda *_args: None,
        lambda root, status: failures.append((root, status)),
        should_bisect=MOD.fragment_batch_failure_is_partitionable,
        on_batch_failure=lambda roots, status: batch_failures.append(
            (tuple(roots), status))) == 0
    assert calls == [(entry, other)]
    assert batch_failures == [
        ((entry, other), "compile-error (toolchain unavailable)")]

    calls.clear()
    batch_failures.clear()

    def capacity_full(roots):
        calls.append(tuple(roots))
        return None, "candidate-capacity: full; 4 existing >= 4"

    assert MOD.compile_batched_fragment_roots(
        {entry, other}, capacity_full,
        lambda *_args: None,
        lambda root, status: failures.append((root, status)),
        should_bisect=MOD.fragment_batch_failure_is_partitionable,
        on_batch_failure=lambda roots, status: batch_failures.append(
            (tuple(roots), status))) == 0
    assert calls == [(entry, other)]
    assert batch_failures == [
        ((entry, other), "candidate-capacity: full; 4 existing >= 4")]

    # Static exact demand is variant-specific: another variant's F entry does
    # not suppress it. Alias demand is narrower and only fills interval holes.
    alias = entry + 0x20
    assert MOD.select_fragment_orphans(
        {entry, alias}, set(), set(), {entry}, {alias},
        set(), []) == [entry, alias]
    assert MOD.select_fragment_orphans(
        {entry, alias}, set(), set(), {entry}, {alias},
        {entry & 0x1FFFFFFF},
        [(alias & 0x1FFFFFFF, (alias & 0x1FFFFFFF) + 8)]) == []
    # A same-address F from another byte variant is deliberately absent from
    # current_variant_entries and therefore cannot suppress executed demand.
    assert MOD.select_fragment_orphans(
        {entry}, {entry}, set(), set(), set(), set(), []) == [entry]
    assert MOD.select_batchable_strong_roots(
        {entry, entry + 4, entry + 8, entry + 12, entry + 16},
        {entry + 4}, {entry + 8}, {entry + 12},
        {(entry + 16) & 0x1FFFFFFF}) == [entry]
    # Mixed live/forced/interval evidence changes only the failure domain; no
    # static-exact authority root may be deferred until after donor freezing.
    batchable, isolated = MOD.partition_strong_root_demands(
        {entry, entry + 4, entry + 8, entry + 12, entry + 16},
        {entry + 4}, {entry + 8}, {entry + 12},
        {(entry + 16) & 0x1FFFFFFF})
    assert batchable == [entry]
    assert isolated == [entry + 4, entry + 8, entry + 12]

    with tempfile.TemporaryDirectory() as td:
        dll = pathlib.Path(td) / "00010000_fragment.dll"
        ranges = pathlib.Path(td) / "00010000_fragment.ranges"
        pair_id = 0x123456789ABCDEF0
        payload = bytes(0x100)
        code_crc = MOD.binascii.crc32(payload[0x20:0x28]) & 0xFFFFFFFF
        manifest = (f"P {pair_id:016X}\n"
                    f"F {entry:08X} {code_crc:08X}\n"
                    f"R {entry:08X} 8\n")
        dll.write_bytes(b"DLL")
        ranges.write_text(manifest)
        old_exports_match = MOD._dll_runtime_exports_match
        try:
            MOD._dll_runtime_exports_match = (
                lambda _path, _abi, expected, entries:
                expected in (pair_id, None) and bool(entries))
            assert MOD.load_shard_entry_set(str(dll)) == {
                entry & 0x1FFFFFFF}
            assert MOD.load_shard_code_ranges(str(dll)) == [
                (entry & 0x1FFFFFFF, (entry & 0x1FFFFFFF) + 8)]
            entries, code_ranges = MOD.load_region_current_variant_coverage(
                td, LOAD & 0x1FFFFFFF, payload, LOAD, len(payload))
            assert entries == {entry & 0x1FFFFFFF}
            assert code_ranges == [
                (entry & 0x1FFFFFFF, (entry & 0x1FFFFFFF) + 8)]
            canonical = pathlib.Path(td) / "00010000_DEADBEEF.dll"
            assert MOD.current_variant_func_id_coverage(
                MOD.load_shard_func_ids(str(canonical)), payload,
                LOAD, len(payload)) == (set(), [])
            changed = bytearray(payload)
            changed[0x20] = 1
            assert MOD.load_region_current_variant_coverage(
                td, LOAD & 0x1FFFFFFF, bytes(changed), LOAD,
                len(changed)) == (set(), [])
            ranges.write_text(manifest.replace(
                f"P {pair_id:016X}", "P 0000000000000001"))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            too_many = (f"P {pair_id:016X}\n"
                        f"F {entry:08X} {code_crc:08X}\n" +
                        "".join(
                            f"R {entry + i * 4:08X} 4\n"
                            for i in range(17)))
            ranges.write_text(too_many)
            assert MOD.load_shard_entry_set(str(dll)) == set()
            no_ranges = (f"P {pair_id:016X}\n"
                         f"F {entry:08X} {code_crc:08X}\n")
            ranges.write_text(no_ranges)
            assert MOD.load_shard_entry_set(str(dll)) == set()
            pairless = (f"F {entry:08X} {code_crc:08X}\n"
                        f"R {entry:08X} 8\n")
            ranges.write_text(pairless)
            assert MOD.load_shard_entry_set(str(dll)) == {
                entry & 0x1FFFFFFF}
            ranges.write_text(
                f"  F {entry:08X} {code_crc:08X}\n\tR {entry:08X} 8  \n")
            assert MOD.load_shard_entry_set(str(dll)) == {
                entry & 0x1FFFFFFF}
            ranges.write_bytes(pairless.replace('\n', '\r\n').encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == {
                entry & 0x1FFFFFFF}
            f_record = f"F {entry:08X} {code_crc:08X}"
            padded_128 = f_record + ' ' * (MOD.MANIFEST_LINE_MAX -
                                            len(f_record))
            ranges.write_bytes(
                (padded_128 + f"\r\nR {entry:08X} 8\r\n").encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == {
                entry & 0x1FFFFFFF}
            ranges.write_bytes(
                (padded_128 + f" \r\nR {entry:08X} 8\r\n").encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_bytes(pairless.replace('\n', '\r').encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_bytes(
                (f"F {entry:08X} {code_crc:08X}\0 junk\n"
                 f"R {entry:08X} 8\n").encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_bytes(("# unknown\0record\n" + pairless).encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_bytes(
                (pairless + "\x1a\nP ZZZ\n").encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_bytes(
                (("#\r" + "x" * MOD.MANIFEST_PHYSICAL_LINE_MAX) +
                 "\n" + pairless).encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_bytes(("P\x1fBAD\n" + pairless).encode('ascii'))
            assert MOD.load_shard_entry_set(str(dll)) == {
                entry & 0x1FFFFFFF}
            ranges.write_text(
                f"F{entry:08X} {code_crc:08X}\nR{entry:08X} 8\n")
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_text(
                f"F {entry:08X}\nR {entry:08X} 8\n")
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_text(
                f"P {pair_id:016X}\nF {entry:08X} {code_crc:08X} junk\n"
                f"R {entry:08X} 8\n")
            assert MOD.load_shard_entry_set(str(dll)) == set()
            outside = 0x80200000
            ranges.write_text(
                f"P {pair_id:016X}\nF {outside:08X} {code_crc:08X}\n"
                f"R {outside:08X} 4\n")
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_text(
                f"F -1 {code_crc:08X}\nR FFFFFFFF 4\n")
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_text(
                f"F 180010000 {code_crc:08X}\nR 180010000 4\n")
            assert MOD.load_shard_entry_set(str(dll)) == set()
            ranges.write_text("#" * (MOD.MANIFEST_LINE_MAX + 1) +
                              "\n" + pairless)
            assert MOD.load_shard_entry_set(str(dll)) == set()
            duplicate = (manifest +
                         f"F {entry:08X} {code_crc ^ 1:08X}\n"
                         f"R {entry:08X} 8\n")
            ranges.write_text(duplicate)
            assert MOD.load_shard_entry_set(str(dll)) == set()
        finally:
            MOD._dll_runtime_exports_match = old_exports_match

        ranges.write_text("EXPECTED\n")
        assert MOD.cached_shard_manifest_status(
            str(dll), None, "EXPECTED\n") == "match"
        assert MOD.cached_shard_manifest_status(
            str(dll), None, "OTHER\n") == "mismatch"
        ranges.unlink()
        assert MOD.cached_shard_manifest_status(
            str(dll), None, "EXPECTED\n") == "missing"

    old_run = MOD.subprocess.run
    seen_seeds = []

    class Args:
        recompiler = "recompiler"
        game_toml = "game.toml"

    class Failed:
        returncode = 1
        stdout = ""
        stderr = "expected stop"

    def stop_after_seeds(cmd, **_kwargs):
        seeds = pathlib.Path(cmd[cmd.index("--seeds") + 1])
        seen_seeds.extend(seeds.read_text().splitlines())
        return Failed()

    try:
        MOD.subprocess.run = stop_after_seeds
        ids, status = MOD.compile_interior_fragment(
            entry, data, LOAD, len(data), LOAD & 0x1FFFFFFF, "unused",
            Args(), {}, {},
            ((LOAD, LOAD + 0x40), (LOAD + 0x80, LOAD + 0x100)),
            (LOAD + 0x200, LOAD + 0x180, LOAD + 0x200))
        assert ids is None and status.startswith("recompiler-error")
    finally:
        MOD.subprocess.run = old_run
    assert seen_seeds == [
        f"producer_range 0x{LOAD:08X} 0x{LOAD + 0x40:08X}",
        f"producer_range 0x{LOAD + 0x80:08X} 0x{LOAD + 0x100:08X}",
        f"cross_call_allow 0x{LOAD + 0x180:08X}",
        f"cross_call_allow 0x{LOAD + 0x200:08X}",
        f"dispatch_root 0x{entry:08X}",
    ]

    seen_seeds.clear()
    try:
        MOD.subprocess.run = stop_after_seeds
        ids, status = MOD.compile_fragment_batch(
            {entry + 0x40, entry}, data, LOAD, len(data),
            LOAD & 0x1FFFFFFF, "unused", Args(), {}, {},
            ((LOAD, LOAD + 0x100),), ())
        assert ids is None and status.startswith("recompiler-error")
    finally:
        MOD.subprocess.run = old_run
    assert seen_seeds == [
        f"producer_range 0x{LOAD:08X} 0x{LOAD + 0x100:08X}",
        f"dispatch_root 0x{entry:08X}",
        f"dispatch_root 0x{entry + 0x40:08X}",
    ]


def check_tcc_runtime_define_parity():
    """TCC shards must use the same release/cycle contract as GCC shards."""
    seen = []
    old_run = MOD.subprocess.run
    old_inc = MOD._bom_free_incdir

    class Result:
        returncode = 0
        stdout = ''
        stderr = ''

    def fake_run(cmd, **_kwargs):
        seen.extend(cmd)
        return Result()

    try:
        MOD.subprocess.run = fake_run
        MOD._bom_free_incdir = lambda path: path
        with tempfile.TemporaryDirectory() as td:
            source = pathlib.Path(td) / 'overlay.c'
            source.write_text('int overlay_test(void) { return 0; }\n')
            assert MOD._compile_dll_tcc(
                str(source), str(pathlib.Path(td) / 'overlay.dll'),
                [td], 3, 'tcc')
            assert b'psx_tcc_ctz' in source.read_bytes()
    finally:
        MOD.subprocess.run = old_run
        MOD._bom_free_incdir = old_inc

    assert '-DPSX_NO_DEBUG_TOOLS' in seen
    assert '-DPSX_ENABLE_BLOCK_CYCLES=1' in seen
    assert '-DPSX_OVERLAY_FLAVOR=3' in seen


def check_real_batched_fragment_publication(recompiler):
    """A poorer same-byte pair stays intact while one richer supplement wins."""
    gcc = (r'C:\msys64\mingw64\bin\gcc.exe'
           if os.path.isfile(r'C:\msys64\mingw64\bin\gcc.exe')
           else shutil.which('gcc'))
    if not gcc:
        return

    data = bytearray(0x100)
    first = LOAD
    second = LOAD + 0x40
    third = LOAD + 0x80
    for offset in (0, 0x40, 0x80):
        put(data, offset, 0x03E00008)  # jr ra
        put(data, offset + 4, 0)
    runtime_include = str(ROOT / 'runtime' / 'include')
    MOD.load_dispatch_preamble(runtime_include)

    class Args:
        game_toml = str(ROOT / 'tools' / 'cycle_testrom' / 'game.toml')
        flavor = 0
        compiler = 'gcc'
        tcc = 'tcc'
        force = False
        cps = True

    args = Args()
    args.recompiler = recompiler
    args.runtime_include = runtime_include
    args.gcc = gcc
    expected_abi = MOD.overlay_abi_tag(runtime_include, args.flavor)
    env = os.environ.copy()
    env['PSX_CPS'] = '1'
    recipe = ((LOAD, LOAD + len(data)),)
    initial_recipe = ((LOAD, LOAD + 0x40),)
    extension = MOD.overlay_ext()

    with tempfile.TemporaryDirectory() as td:
        ids, status = MOD.compile_fragment_batch(
            {first}, bytes(data), LOAD, len(data), LOAD & 0x1FFFFFFF,
            td, args, env, {}, initial_recipe, ())
        assert ids and status == 'built'
        initial_dlls = list(pathlib.Path(td).glob(f'*{extension}'))
        assert len(initial_dlls) == 1
        initial_dll = initial_dlls[0]
        initial_ranges = initial_dll.with_suffix('.ranges')
        initial_pair = (initial_dll.read_bytes(), initial_ranges.read_bytes())

        current_entries, _ranges = MOD.load_region_current_variant_coverage(
            td, LOAD & 0x1FFFFFFF, bytes(data), LOAD, len(data),
            expected_abi)
        assert current_entries == {first & 0x1FFFFFFF}
        requested_batches = []

        def compile_missing(roots):
            requested_batches.append(tuple(roots))
            return MOD.compile_fragment_batch(
                roots, bytes(data), LOAD, len(data), LOAD & 0x1FFFFFFF,
                td, args, env, {}, recipe, ())

        def warm(_roots, func_ids, _status):
            current_entries.update(
                entry & 0x1FFFFFFF for entry, _crc, _ranges in func_ids)

        assert MOD.compile_batched_fragment_roots(
            {first, second, third}, compile_missing, warm,
            lambda root, reason: (_ for _ in ()).throw(
                AssertionError((root, reason))),
            lambda root: (root & 0x1FFFFFFF) not in current_entries,
            MOD.fragment_batch_failure_is_partitionable) == 1
        assert requested_batches == [(second, third)]
        assert (initial_dll.read_bytes(), initial_ranges.read_bytes()) == initial_pair
        final_dlls = list(pathlib.Path(td).glob(f'*{extension}'))
        assert len(final_dlls) == 2
        supplemental = next(dll for dll in final_dlls if dll != initial_dll)
        assert {second & 0x1FFFFFFF, third & 0x1FFFFFFF} <= \
            MOD.load_shard_entry_set(str(supplemental), expected_abi)
        final_entries, _ranges = MOD.load_region_current_variant_coverage(
            td, LOAD & 0x1FFFFFFF, bytes(data), LOAD, len(data),
            expected_abi)
        assert {first & 0x1FFFFFFF, second & 0x1FFFFFFF,
                third & 0x1FFFFFFF} <= final_entries
        assert all(MOD.compiled_shard_complete(str(dll), expected_abi)
                   for dll in pathlib.Path(td).glob(f'*{extension}'))


def check_real_hosted_fragment_publication(recompiler):
    """Only exact cached-owner identity may authorize a hosted alias pair."""
    gcc = (r'C:\msys64\mingw64\bin\gcc.exe'
           if os.path.isfile(r'C:\msys64\mingw64\bin\gcc.exe')
           else shutil.which('gcc'))
    if not gcc:
        return
    data = bytearray(0x100)
    host = LOAD
    target = LOAD + 0x10
    put(data, 0x00, 0x1480000B)  # bne a0,zero,LOAD+0x30
    put(data, 0x04, 0)
    put(data, 0x08, 0x01000008)  # jr t0
    put(data, 0x0C, 0)
    put(data, 0x10, 0x24420001)
    put(data, 0x14, 0x03E00008)
    put(data, 0x18, 0)
    put(data, 0x30, 0x03E00008)
    put(data, 0x34, 0)
    host2 = LOAD + 0x80
    target2 = host2 + 0x10
    put(data, 0x80, 0x1480000B)
    put(data, 0x84, 0)
    put(data, 0x88, 0x01000008)
    put(data, 0x8C, 0)
    put(data, 0x90, 0x24420002)
    put(data, 0x94, 0x03E00008)
    put(data, 0x98, 0)
    put(data, 0xB0, 0x03E00008)
    put(data, 0xB4, 0)
    runtime_include = str(ROOT / 'runtime' / 'include')
    MOD.load_dispatch_preamble(runtime_include)

    class Args:
        game_toml = str(ROOT / 'tools' / 'cycle_testrom' / 'game.toml')
        flavor = 0
        compiler = 'gcc'
        tcc = 'tcc'
        force = False
        cps = True

    args = Args()
    args.recompiler = recompiler
    args.runtime_include = runtime_include
    args.gcc = gcc
    expected_abi = MOD.overlay_abi_tag(runtime_include, args.flavor)
    env = os.environ.copy()
    env['PSX_CPS'] = '1'
    recipe = ((LOAD, LOAD + len(data)),)

    with tempfile.TemporaryDirectory() as td:
        owner_ids, status = MOD.compile_fragment_batch(
            {host, host2}, bytes(data), LOAD, len(data), LOAD & 0x1FFFFFFF,
            td, args, env, {}, recipe, ())
        assert owner_ids and status == 'built'
        owners = {identity[0]: identity for identity in owner_ids
                  if identity[0] in (host, host2)}
        assert set(owners) == {host, host2}
        assert not ({target, target2} & {
            entry for entry, _crc, _ranges in owner_ids})
        owner_dlls = set(pathlib.Path(td).glob(f'*{MOD.overlay_ext()}'))
        spec = {
            'host': host,
            'owner_identity': owners[host],
            'host_reason': 'STATIC_DISCOVERY_ROOT',
        }
        spec2 = {
            'host': host2,
            'owner_identity': owners[host2],
            'host_reason': 'STATIC_DISCOVERY_ROOT',
        }
        hosted_ids, status = MOD.compile_fragment_batch(
            {target, target2}, bytes(data), LOAD, len(data),
            LOAD & 0x1FFFFFFF, td, args, env, {}, recipe, (),
            hosted_owners={target: spec, target2: spec2})
        assert hosted_ids and status == 'built', status
        by_entry = {
            entry: (crc, tuple(ranges))
            for entry, crc, ranges in hosted_ids
        }
        assert by_entry[target] == by_entry[host]
        assert by_entry[target2] == by_entry[host2]
        hosted_dlls = set(pathlib.Path(td).glob(
            f'*{MOD.overlay_ext()}')) - owner_dlls
        assert len(hosted_dlls) == 1
        hosted_dll = hosted_dlls.pop()
        assert MOD.HOSTED_MANIFEST_MARKER in \
            hosted_dll.with_suffix('.ranges').read_text().splitlines()
        assert MOD.manifest_has_hosted_provenance(
            MOD.HOSTED_MANIFEST_MARKER + '\n')
        assert MOD.manifest_has_hosted_provenance(
            MOD.HOSTED_MANIFEST_MARKER + '\r\n')
        assert not MOD.manifest_has_hosted_provenance(
            MOD.HOSTED_MANIFEST_MARKER + '\rjunk\n')
        assert MOD.manifest_declares_supplemental_provenance(
            '# psxrecomp overlay provenance unknown-v9\r\n')
        assert MOD.load_shard_func_ids(
            str(hosted_dll), expected_abi,
            include_supplemental=False) == []
        authority_ids = MOD.load_region_current_variant_func_ids(
            td, LOAD & 0x1FFFFFFF, bytes(data), LOAD, len(data),
            expected_abi, include_supplemental=False)
        assert not ({target, target2} & {
            entry for entry, _crc, _ranges in authority_ids})
        coverage_ids = MOD.load_region_current_variant_func_ids(
            td, LOAD & 0x1FFFFFFF, bytes(data), LOAD, len(data),
            expected_abi)
        assert {target, target2} <= {
            entry for entry, _crc, _ranges in coverage_ids}
        assert MOD.overlay_pair_id(
            'same', hosted_ids) != MOD.overlay_pair_id(
                'same', hosted_ids, MOD.HOSTED_MANIFEST_PROVENANCE)
        assert MOD.fragment_shard_key(hosted_ids) != \
            MOD.fragment_shard_key(
                hosted_ids, MOD.HOSTED_MANIFEST_PROVENANCE)
        assert hosted_dll.stem.endswith(
            f'{MOD.fragment_shard_key(hosted_ids, MOD.HOSTED_MANIFEST_PROVENANCE):08X}')

        dll_count = len(list(pathlib.Path(td).glob(f'*{MOD.overlay_ext()}')))
        cached_ids, cached_status = MOD.compile_fragment_batch(
            {target, target2}, bytes(data), LOAD, len(data),
            LOAD & 0x1FFFFFFF, td, args, env, {}, recipe, (),
            hosted_owners={target: spec, target2: spec2})
        assert cached_status == 'cached' and cached_ids == hosted_ids
        assert len(list(pathlib.Path(td).glob(
            f'*{MOD.overlay_ext()}'))) == dll_count
        orphan_before = set(pathlib.Path(td).glob(f'*{MOD.overlay_ext()}'))
        orphan_ids, orphan_status = MOD.compile_interior_fragment(
            target, bytes(data), LOAD, len(data), LOAD & 0x1FFFFFFF,
            td, args, env, {}, recipe, ())
        assert orphan_ids and orphan_status == 'built'
        orphan_dlls = set(pathlib.Path(td).glob(
            f'*{MOD.overlay_ext()}')) - orphan_before
        assert len(orphan_dlls) == 1
        orphan_dll = orphan_dlls.pop()
        orphan_manifest = orphan_dll.with_suffix('.ranges').read_text()
        assert MOD.manifest_provenance(orphan_manifest) == \
            MOD.ORPHAN_MANIFEST_PROVENANCE
        assert MOD.load_shard_func_ids(
            str(orphan_dll), 14, include_supplemental=False) == []
        dll_count += 1
        bad_spec = dict(spec, owner_identity=(
            owners[host][0], owners[host][1] ^ 1, owners[host][2]))
        rejected, reason = MOD.compile_fragment_batch(
            {target}, bytes(data), LOAD, len(data), LOAD & 0x1FFFFFFF,
            td, args, env, {}, recipe, (), hosted_owners={target: bad_spec})
        assert rejected is None
        assert reason.startswith('hosted-entry-audit:'), reason
        assert len(list(pathlib.Path(td).glob(
            f'*{MOD.overlay_ext()}'))) == dll_count


def check_full_hosted_fixed_point(recompiler):
    """A clean two-variant CLI build must make its second run a true no-op."""
    gcc = (r'C:\msys64\mingw64\bin\gcc.exe'
           if os.path.isfile(r'C:\msys64\mingw64\bin\gcc.exe')
           else shutil.which('gcc'))
    if not gcc:
        return
    host = LOAD
    target = LOAD + 0x10

    donor = bytearray(0x100)
    put(donor, 0x00, 0x03E00008)  # jr ra
    put(donor, 0x04, 0)
    put(donor, 0x10, 0x27BDFFF0)  # addiu sp,sp,-16
    put(donor, 0x14, 0xAFBF000C)  # sw ra,12(sp)
    put(donor, 0x18, 0x03E00008)  # jr ra
    put(donor, 0x1C, 0x27BD0010)  # addiu sp,sp,16

    recipient = bytearray(0x100)
    put(recipient, 0x00, 0x1480000B)  # bne a0,zero,LOAD+0x30
    put(recipient, 0x04, 0)
    put(recipient, 0x08, 0x01000008)  # jr t0
    put(recipient, 0x0C, 0)
    put(recipient, 0x10, 0x24420001)
    put(recipient, 0x14, 0x03E00008)
    put(recipient, 0x18, 0)
    put(recipient, 0x30, 0x03E00008)
    put(recipient, 0x34, 0)

    def capture(data, function_entries, dispatch_entries, static_dispatch,
                static_discovery, executed=()):
        encoded = MOD.base64.b64encode(bytes(data)).decode('ascii')
        to_hex = lambda entries: [f'0x{entry:08X}' for entry in entries]
        return {
            'schema': 'psxrecomp overlay capture v2',
            'load_addr': f'0x{LOAD:08X}',
            'size': len(data),
            'bytes_b64': encoded,
            'executed_pcs': to_hex(executed),
            'dispatch_entry_pcs': to_hex(dispatch_entries),
            'static_dispatch_entry_pcs': to_hex(static_dispatch),
            'function_entry_pcs': to_hex(function_entries),
            'seeds': to_hex(function_entries),
            'static_discovery_entry_pcs': to_hex(static_discovery),
        }

    captures = [
        # Static+executed overlap exercises the pre-freeze singleton path.
        capture(donor, [target], [target], [target], [target], [target]),
        # The same address is only an organic block inside this rooted host.
        capture(recipient, [host], [host], [], [host]),
    ]
    runtime_include = str(ROOT / 'runtime' / 'include')
    game_toml = str(ROOT / 'tools' / 'cycle_testrom' / 'game.toml')
    script = str(ROOT / 'tools' / 'compile_overlays.py')

    with tempfile.TemporaryDirectory() as td:
        captures_path = pathlib.Path(td) / 'captures.json'
        captures_path.write_text(MOD.json.dumps(captures), encoding='utf-8')
        cache_root = pathlib.Path(td) / 'cache'
        command = [
            sys.executable, script,
            '--captures', str(captures_path),
            '--game-toml', game_toml,
            '--recompiler', recompiler,
            '--runtime-include', runtime_include,
            '--gcc', gcc,
            '--out-dir', str(cache_root),
            '--jobs', '1',
        ]
        env = os.environ.copy()
        env['PATH'] = os.path.dirname(gcc) + os.pathsep + env.get('PATH', '')
        first = subprocess.run(
            command, cwd=str(ROOT), env=env, capture_output=True, text=True,
            timeout=120)
        assert first.returncode == 0, first.stdout + first.stderr
        manifests = list(cache_root.rglob('*.ranges'))
        hosted_manifests = [
            path for path in manifests
            if MOD.manifest_has_hosted_provenance(
                path.read_text(encoding='ascii'))
        ]
        assert hosted_manifests, first.stdout
        alias_found = False
        for manifest in hosted_manifests:
            _pair, funcs = MOD.parse_runtime_shard_manifest(
                manifest.read_text(encoding='ascii'), require_pair=True)
            for entry, _crc, ranges in funcs:
                if (entry == target and len(ranges) == 1 and
                        ranges[0][0] == host):
                    alias_found = True
        assert alias_found, first.stdout

        def inventory():
            return {
                str(path.relative_to(cache_root)): MOD.hashlib.sha256(
                    path.read_bytes()).hexdigest()
                for path in cache_root.rglob('*')
                if (path.is_file() and
                    path.suffix in (MOD.overlay_ext(), '.ranges') and
                    not path.name.startswith('.'))
            }

        before = inventory()
        second = subprocess.run(
            command, cwd=str(ROOT), env=env, capture_output=True, text=True,
            timeout=120)
        assert second.returncode == 0, second.stdout + second.stderr
        assert inventory() == before
        assert 'PSX_SHARD_RESULT ok=0 failed=0 ' in second.stdout, second.stdout


def check_full_candidate_cli_fastpath(recompiler):
    """An already-full single-tier cache must bypass every compile recipe."""
    gcc = (r'C:\msys64\mingw64\bin\gcc.exe'
           if os.path.isfile(r'C:\msys64\mingw64\bin\gcc.exe')
           else shutil.which('gcc'))
    if not gcc:
        return
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        runtime_include = tmp / 'include'
        shutil.copytree(ROOT / 'runtime' / 'include', runtime_include)
        api = runtime_include / 'overlay_api.h'
        api_text = api.read_text(encoding='utf-8')
        assert '#define PSX_OVERLAY_CANDIDATE_CAP 32768' in api_text
        api.write_text(api_text.replace(
            '#define PSX_OVERLAY_CANDIDATE_CAP 32768',
            '#define PSX_OVERLAY_CANDIDATE_CAP 1'), encoding='utf-8')

        game_toml = ROOT / 'tools' / 'cycle_testrom' / 'game.toml'
        cache_root = tmp / 'cache'
        leaf = (cache_root / 'CYCT-00101' / 'gcc' / MOD.cache_arch_abi() /
                f'cg{MOD.codegen_ver(str(runtime_include))}_'
                f'{MOD.codegen_hash(str(runtime_include)):08x}')
        leaf.mkdir(parents=True)
        pair_id = 0x123456789ABCDEF0
        captured_bytes = b'\x08\x00\xE0\x03\x00\x00\x00\x00'
        code_crc = MOD.binascii.crc32(captured_bytes) & 0xFFFFFFFF
        ext = MOD.overlay_ext()
        dll = leaf / f'00010000_{code_crc:08X}{ext}'
        source = tmp / 'prefill.c'
        abi = MOD.overlay_abi_tag(str(runtime_include), 0)
        export = ('__declspec(dllexport)'
                  if os.name == 'nt' else
                  '__attribute__((visibility("default")))')
        source.write_text(
            '#include <stdint.h>\n'
            f'#define EX {export}\n'
            f'EX int overlay_abi(void){{return {abi};}}\n'
            f'EX uint64_t overlay_pair_id(void){{return UINT64_C('
            f'0x{pair_id:016X});}}\n'
            'EX void overlay_init(const void*p){(void)p;}\n'
            'EX void overlay_flush_cycles(void){}\n'
            'EX void func_80010000(void*p){(void)p;}\n',
            encoding='utf-8')
        subprocess.run(
            [gcc, '-shared', str(source), '-o', str(dll)],
            check=True, capture_output=True, text=True)
        manifest = dll.with_suffix('.ranges')
        manifest.write_text(MOD.overlay_ranges_text([
            (LOAD, code_crc, ((LOAD, 8),)),
        ], pair_id), encoding='ascii')
        assert len(MOD.load_shard_func_ids(str(dll), abi)) == 1

        capture_path = tmp / 'captures.json'
        capture = {
            'schema': 'psxrecomp overlay capture v2',
            'load_addr': f'0x{LOAD:08X}',
            'size': 8,
            'bytes_b64': MOD.base64.b64encode(captured_bytes).decode(),
            'executed_pcs': [f'0x{LOAD:08X}'],
            'dispatch_entry_pcs': [f'0x{LOAD:08X}'],
            'function_entry_pcs': [f'0x{LOAD:08X}'],
            'seeds': [f'0x{LOAD:08X}'],
        }
        resident_capture = dict(capture)
        resident_capture.update({
            'producer': MOD.BIOS_RESIDENT_PRODUCER,
            'producer_name': 'synthetic resident fixed-point test',
            'bios_sha256': '71af94d1',
        })
        capture_path.write_text(
            MOD.json.dumps([resident_capture, capture]), encoding='utf-8')
        before = {
            path.name: (MOD.hashlib.sha256(path.read_bytes()).hexdigest(),
                        path.stat().st_mtime_ns)
            for path in (dll, manifest)
        }
        command = [
            sys.executable, str(ROOT / 'tools' / 'compile_overlays.py'),
            '--captures', str(capture_path),
            '--game-toml', str(game_toml),
            '--recompiler', recompiler,
            '--runtime-include', str(runtime_include),
            '--gcc', gcc,
            '--out-dir', str(cache_root),
            '--jobs', '1',
        ]
        env = os.environ.copy()
        env.pop('PSX_OVERLAY_CACHE_DIR', None)
        env.pop('PSX_OVERLAY_CAPTURES', None)
        env['PATH'] = os.path.dirname(gcc) + os.pathsep + env.get('PATH', '')
        run = subprocess.run(
            command, cwd=str(ROOT), env=env, capture_output=True, text=True,
            timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        assert 'Overlay candidate fixed point: 1/1' in run.stdout, run.stdout
        assert ('processed 1 BIOS resident recipe(s) and skipped 1 ordinary '
                'capture recipe(s)') in run.stdout, run.stdout
        assert ('PSX_SHARD_RESULT ok=0 failed=0 skipped=1 '
                'capacity_fastpath=1') in run.stdout, run.stdout
        assert '  compile:' not in run.stdout
        after = {
            path.name: (MOD.hashlib.sha256(path.read_bytes()).hexdigest(),
                        path.stat().st_mtime_ns)
            for path in (dll, manifest)
        }
        assert after == before
        marker = dll.with_suffix('.resident')
        marker_bytes = marker.read_bytes()
        assert MOD.json.loads(marker_bytes.decode('utf-8'))['schema'] == \
            MOD.BIOS_RESIDENT_MARKER
        sentinel_ns = 946684800_000_000_000
        os.utime(marker, ns=(sentinel_ns, sentinel_ns))
        marker_mtime = marker.stat().st_mtime_ns
        rerun = subprocess.run(
            command, cwd=str(ROOT), env=env, capture_output=True, text=True,
            timeout=30)
        assert rerun.returncode == 0, rerun.stdout + rerun.stderr
        assert marker.read_bytes() == marker_bytes
        assert marker.stat().st_mtime_ns == marker_mtime


def check_resident_marker_fixed_point():
    with tempfile.TemporaryDirectory() as td:
        dll = pathlib.Path(td) / '0000DF80_4EE6AC69.dll'
        dll.write_bytes(b'dll')
        cap = {
            'producer': MOD.BIOS_RESIDENT_PRODUCER,
            'producer_name': 'SCPH-1001 resident helper',
            'bios_sha256': '71af94d1',
        }
        MOD.update_bios_resident_marker(str(dll), cap)
        marker = dll.with_suffix('.resident')
        first_bytes = marker.read_bytes()
        parsed = MOD.json.loads(first_bytes.decode('utf-8'))
        assert parsed['schema'] == MOD.BIOS_RESIDENT_MARKER
        assert parsed['producer_name'] == cap['producer_name']
        sentinel_ns = 946684800_000_000_000
        os.utime(marker, ns=(sentinel_ns, sentinel_ns))
        first_mtime = marker.stat().st_mtime_ns
        MOD.update_bios_resident_marker(str(dll), cap)
        assert marker.read_bytes() == first_bytes
        assert marker.stat().st_mtime_ns == first_mtime

        marker.write_text('{malformed', encoding='utf-8')
        MOD.update_bios_resident_marker(str(dll), cap)
        assert marker.read_bytes() == first_bytes

        MOD.update_bios_resident_marker(str(dll), {'producer': 'ordinary'})
        assert not marker.exists()


def main():
    default_recompiler = ROOT / "recompiler" / "build" / "psxrecomp-game.exe"
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", default=str(default_recompiler))
    args = parser.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit(f"recompiler not found: {args.recompiler}")

    check_composite_call_boundaries()
    check_bounded_jump_table_discovery()
    check_static_discovery_provenance()
    check_static_dispatch_provenance()
    check_owned_direct_call_is_interior()
    check_discovered_host_owns_same_round_call_target()
    check_later_discovered_host_retracts_interior_root()
    check_recompiler_discovered_host_ownership(args.recompiler)
    check_recompiler_explicit_hosted_interior(args.recompiler)
    check_recompiler_hosted_interior_parser(args.recompiler)
    check_ownership_invalidation_and_producer_boundaries()
    check_recompiler_unreachable_jal_not_alias(args.recompiler)
    check_recomputed_evidence_and_unconditional_branch()
    check_t2_shaped_retained_partition_conflict(args.recompiler)
    check_static_alias_recipe()
    check_optional_enrichment_fallback()
    check_forward_branch_root()
    check_recompiler_composite_contract(args.recompiler)
    check_recompiler_pointer_table_call_root(args.recompiler)
    check_recompiler_pointer_table_alias(args.recompiler)
    check_retained_alias_contract(args.recompiler)
    check_atomic_dll_publication()
    check_candidate_capacity_publication()
    check_interior_fragment_contract()
    check_tcc_runtime_define_parity()
    check_real_batched_fragment_publication(args.recompiler)
    check_real_hosted_fragment_publication(args.recompiler)
    check_full_hosted_fixed_point(args.recompiler)
    check_full_candidate_cli_fastpath(args.recompiler)
    check_resident_marker_fixed_point()

    data = bytearray(0x200)

    # A framed root directly calls a frameless leaf.  JAL is the boundary
    # proof; the leaf deliberately has neither a prologue nor a preceding
    # return.
    put(data, 0x00, 0x27BDFFF0)
    put(data, 0x04, jal(LOAD + 0x40))
    put(data, 0x08, 0x00000000)
    put(data, 0x0C, 0x03E00008)
    put(data, 0x10, 0x27BD0010)
    put(data, 0x40, 0x24020001)
    put(data, 0x44, 0x03E00008)
    put(data, 0x48, 0x00000000)

    cap = {
        "schema": "psxrecomp overlay capture v2",
        "load_addr": f"0x{LOAD:08X}",
        "size": len(data),
        "executed_pcs": [],
        "dispatch_entry_pcs": [f"0x{LOAD:08X}"],
        "function_entry_pcs": [f"0x{LOAD:08X}"],
        "seeds": [f"0x{LOAD:08X}"],
    }
    seeds, audit = MOD.classify_overlay_seeds(
        cap, bytes(data), LOAD, len(data), 0, {})
    assert audit["included_reasons"][LOAD + 0x40] == "DIRECT_JAL_TARGET"
    assert f"call_root 0x{LOAD + 0x40:08X}" in seeds

    # A constant-register JR tail-calls backward to a frameless leaf.
    put(data, 0x100, 0x27BDFFF0)
    put(data, 0x104, 0x3C088001)  # lui $t0,0x8001
    put(data, 0x108, 0x25080040)  # addiu $t0,$t0,0x40
    put(data, 0x10C, 0x01000008)  # jr $t0
    put(data, 0x110, 0x27BD0010)
    walk = MOD._walk_overlay_function(
        bytes(data), LOAD, len(data), LOAD + 0x100, LOAD + len(data))
    assert walk["static_indirect_targets"] == {LOAD + 0x40}

    check_padded_return_boundary(args.recompiler)

    print("ALL PASS")


if __name__ == "__main__":
    sys.exit(main())
