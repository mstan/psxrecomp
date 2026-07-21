#!/usr/bin/env python3
"""AOT static-coverage recall report + gap manifest.

Answers both "which played manifest entries did static extraction reproduce?"
and "which played entry PCs lie in any compiled static code interval?" The
latter is only an address-containment potential metric: it does not prove that
the containing manifest belongs to the live content variant, passes its whole-
function CRC guard, or can dispatch at that interior PC.

Two ground-truth "needed" sources:
  * a played/coverage VAULT cache dir (.ranges from a full playthrough) — the most
    complete needed-set we have; the primary recall benchmark.
  * a runtime CAPTURES json (executed_pcs from a live session) — what a specific
    drive exercised; a live spot-check.

Recall is reported at three strengths:
  * entry-level      : same function ENTRY address discovered (did we find it?)
  * interval potential: played entry PC lies in an R range from some static AOT
  * entry+code_crc    : same entry AND same manifest code CRC candidate

Usage:
  coverage_report.py --static <static-cache-dir> --vault <vault-cache-dir>
                     [--bios-dispatch <SCPHxxxx_dispatch.c>]
                     [--captures <runtime_overlay_captures.json>]...
                     [--addendum <overlay_captures.addendum.jsonl>]
                     [--prior-report <gaps.json> --assume-static-superset]
                     --out-md <path.md> --out-json <path.json> [--game <id>]
Cache dirs are scanned recursively for *.ranges; a captures json is the v2 schema.
"""
import argparse, bisect, os, sys, json, re, glob, zlib
from collections import defaultdict

MASK = 0x1FFFFFFF
FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MAX_UNVERIFIED_SNAPSHOT_BYTES = 32 * 1024 * 1024
MAX_UNVERIFIED_TOTAL_BYTES = 2 * 1024 * 1024 * 1024

def norm(a): return a & MASK

def region_of(a):
    """Page-region label for grouping (aligns to the overlay window bases)."""
    return norm(a) & 0xFFFFF000

def merge_intervals(intervals):
    merged = []
    for lo, hi in sorted(intervals):
        if not merged or lo > merged[-1][1]:
            merged.append([lo, hi])
        elif hi > merged[-1][1]:
            merged[-1][1] = hi
    return [(lo, hi) for lo, hi in merged]

def _zero_filled_crc(size):
    crc = 0
    zeroes = bytes(min(size, 65536))
    remaining = size
    while remaining:
        take = min(remaining, len(zeroes))
        crc = zlib.crc32(zeroes[:take], crc)
        remaining -= take
    return crc & 0xFFFFFFFF

def parse_ranges_dir(d, with_audit=False):
    """Return entries/ranges, rejecting proven all-zero data-as-code records.

    A valid function cannot be an entire compiled range of MIPS NOP words: it
    has no return or transfer. Historical runtime compilers nevertheless minted
    such ranges from observed sequential PCs. The v2 manifest has enough proof
    to quarantine them without needing the original capture bytes: F carries
    the range CRC and its following R carries the exact byte count.
    """
    # One virtual entry can legitimately have several content-keyed overlay
    # variants.  The runtime retains all of them and selects by the owning
    # function's whole-range CRC, so collapsing this to entry -> one arbitrary
    # CRC makes the scoreboard depend on glob traversal order.
    ent = defaultdict(set)
    intervals = []
    quarantined = set()
    for rf in glob.glob(os.path.join(d, '**', '*.ranges'), recursive=True):
        pending = None
        with open(rf, errors='ignore') as manifest:
            for line in manifest:
                m = re.match(r'F\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)', line)
                if m:
                    if pending is not None:
                        ent[pending[0]].add(pending[1])
                    pending = (norm(int(m.group(1), 16)), int(m.group(2), 16))
                    continue
                m = re.match(r'R\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)', line)
                if m:
                    lo = norm(int(m.group(1), 16))
                    size = int(m.group(2), 16)
                    if (pending is not None and size > 0 and size % 4 == 0 and
                            pending[1] == _zero_filled_crc(size)):
                        quarantined.add((pending[0], pending[1], size))
                    else:
                        if pending is not None:
                            ent[pending[0]].add(pending[1])
                        intervals.append((lo, lo + size))
                    pending = None
        if pending is not None:
            ent[pending[0]].add(pending[1])
    result = (dict(ent), merge_intervals(intervals))
    if not with_audit:
        return result
    audit = [
        {'entry': f'0x{entry | 0x80000000:08X}',
         'code_crc': f'{crc:08X}', 'size': size}
        for entry, crc, size in sorted(quarantined)
    ]
    return result + (audit,)

def _c_initializer(text, declaration_pattern):
    """Return the braced initializer following a unique C declaration."""
    match = re.search(declaration_pattern, text)
    if not match:
        raise ValueError(f'missing generated-C declaration: {declaration_pattern}')
    start = text.find('{', match.end())
    if start < 0:
        raise ValueError('generated-C declaration has no initializer')
    depth = 0
    for pos in range(start, len(text)):
        if text[pos] == '{':
            depth += 1
        elif text[pos] == '}':
            depth -= 1
            if depth == 0:
                return text[start + 1:pos]
    raise ValueError('unterminated generated-C initializer')

def parse_bios_dispatch(path):
    """Return base-BIOS dispatch entries and relocated kernel body ranges.

    The generated dispatcher has many unrelated numeric initializers, so parse
    only the two named tables instead of matching address triples globally.
    """
    with open(path, encoding='utf-8', errors='ignore') as generated:
        text = generated.read()
    dispatch = _c_initializer(
        text, r'\bDispatchEntry\s+dispatch_table\s*\[[^]]*\]\s*=')
    bodies = _c_initializer(
        text, r'\bPsxKernelBody\s+psx_bios_kernel_bodies\s*\[[^]]*\]\s*=')
    entries = {
        norm(int(value, 16))
        for value in re.findall(
            r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*func_[0-9A-Fa-f]+\s*\}', dispatch)
    }
    intervals = []
    for _, lo, hi in re.findall(
            r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*'
            r'0x([0-9A-Fa-f]+)u?\s*,\s*0x([0-9A-Fa-f]+)u?\s*\}', bodies):
        lo, hi = norm(int(lo, 16)), norm(int(hi, 16))
        if hi > lo:
            intervals.append((lo, hi))
    try:
        native_stubs = _c_initializer(
            text, r'\bPsxNativeStub\s+psx_bios_native_stubs\s*\[[^]]*\]\s*=')
    except ValueError:
        native_stubs = ''
    for key, lo, hi in re.findall(
            r'\{\s*0x([0-9A-Fa-f]+)u?\s*,\s*'
            r'0x([0-9A-Fa-f]+)u?\s*,\s*0x([0-9A-Fa-f]+)u?\s*\}', native_stubs):
        key, lo, hi = norm(int(key, 16)), norm(int(lo, 16)), norm(int(hi, 16))
        if hi > lo and lo <= key < hi:
            entries.add(key)
            intervals.append((lo, hi))
    if not entries or not intervals:
        raise ValueError('generated BIOS dispatcher contained an empty static table')
    return entries, merge_intervals(intervals)

def parse_captures_executed(path):
    """-> set of FUNCTION-ENTRY addrs (masked) the runtime dispatched to.
    Use dispatch/function entries only — executed_pcs is per-basic-block PCs
    (many per function), which is not comparable to the vault's function-entry
    granularity and would pollute the recall denominator."""
    ent = set()
    for cap in json.load(open(path)):
        for key in ('dispatch_entry_pcs', 'function_entry_pcs'):
            for a in cap.get(key, []):
                ent.add(norm(int(a, 16)))
    return ent

def _fnv64_bytes(data, value=FNV64_OFFSET):
    for byte in data:
        value ^= byte
        value = (value * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value

def _fnv64_file(path):
    value = FNV64_OFFSET
    with open(path, 'rb') as source:
        while True:
            chunk = source.read(65536)
            if not chunk:
                break
            value = _fnv64_bytes(chunk, value)
    return value

def parse_addendum_entries(path):
    """Union dispatch/function entries from verified v1/v2 history records.

    Runtime v2 snapshots are cumulative within one session: the capture tables
    only grow, and every history record references a full immutable snapshot.
    For a well-formed session, verify the newest snapshot and cheaply prove that
    every older entry set is a subset before skipping their expensive FNV pass.
    Every candidate must still have a syntactically valid signature, and the
    unverified reads are size-bounded. If content is non-cumulative, missing, or
    unreadable, fall back to verifying and unioning every valid record as before.

    Records without trustworthy session/sequence metadata remain independent,
    preserving the conservative behavior for hand-written and legacy history.
    """
    entries = set()
    seen_refs = set()
    audit = {'v1_records': 0, 'v2_records': 0, 'invalid_records': 0,
             'duplicate_refs': 0, 'verified_snapshots': 0,
             'unverified_superseded_records': 0,
             'unverified_superseded_bytes': 0}
    session_records = defaultdict(list)
    independent_records = []
    all_v2_records = []
    verified_probe = {}
    unverified_bytes_read = 0

    def capture_entries(captures):
        found = set()
        if not isinstance(captures, list):
            return found
        for cap in captures:
            if not isinstance(cap, dict):
                continue
            for key in ('dispatch_entry_pcs', 'function_entry_pcs'):
                for addr in cap.get(key, []):
                    found.add(norm(int(addr, 16) if isinstance(addr, str)
                                   else int(addr)))
        return found

    def add_captures(captures):
        entries.update(capture_entries(captures))

    def snapshot_fields(record):
        snapshot = record.get('snapshot')
        expected = str(record.get('fnv64', '')).upper()
        if not isinstance(snapshot, str) or not snapshot:
            return None, expected, None
        if not os.path.isabs(snapshot):
            snapshot = os.path.join(os.path.dirname(path), snapshot)
        snapshot = os.path.abspath(snapshot)
        return snapshot, expected, (os.path.normcase(snapshot), expected)

    def read_snapshot_bytes(snapshot, max_bytes=None):
        with open(snapshot, 'rb') as source:
            if max_bytes is not None and os.fstat(source.fileno()).st_size > max_bytes:
                raise ValueError('snapshot exceeds unverified parse bound')
            data = source.read() if max_bytes is None else source.read(max_bytes + 1)
        if max_bytes is not None and len(data) > max_bytes:
            raise ValueError('snapshot exceeds unverified parse bound')
        return data

    def entries_from_bytes(data):
        return capture_entries(json.loads(data))

    with open(path, encoding='utf-8', errors='replace') as history:
        for lineno, line in enumerate(history, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except Exception as exc:
                print(f'  warn: ignoring invalid addendum line {lineno} ({exc})')
                audit['invalid_records'] += 1
                continue
            if not isinstance(record, dict):
                audit['invalid_records'] += 1
                continue
            schema = record.get('schema')
            if schema == 'psxrecomp overlay capture addendum v1':
                audit['v1_records'] += 1
                add_captures(record.get('captures'))
                continue
            if schema != 'psxrecomp overlay capture addendum v2':
                audit['invalid_records'] += 1
                continue
            audit['v2_records'] += 1
            all_v2_records.append((lineno, record))
            game = record.get('game')
            session = record.get('session')
            sequence = record.get('sequence')
            if (isinstance(game, str) and game and
                    isinstance(session, str) and session and
                    isinstance(sequence, int) and not isinstance(sequence, bool) and
                    sequence > 0):
                session_records[(game, session)].append(
                    (sequence, lineno, record))
            else:
                independent_records.append((lineno, record))

    # Preserve the original duplicate-reference audit even when cumulative
    # session records later avoid per-file verification.
    referenced = set()
    for _, record in all_v2_records:
        _, _, ref = snapshot_fields(record)
        if ref is not None:
            if ref in referenced:
                audit['duplicate_refs'] += 1
            else:
                referenced.add(ref)

    # A valid runtime session has unique, monotonically increasing sequence
    # numbers in append order.  Anything else is treated independently rather
    # than assuming the producer's cumulative-snapshot contract.
    candidate_groups = []
    for records in session_records.values():
        append_order = [sequence for sequence, _, _ in records]
        if (len(set(append_order)) != len(append_order) or
                append_order != sorted(append_order)):
            independent_records.extend((lineno, record)
                                       for _, lineno, record in records)
        else:
            candidate_groups.append(list(reversed(records)))
    candidate_groups.extend([[(0, lineno, record)]
                             for lineno, record in independent_records])

    for candidates in candidate_groups:
        # Metadata is not itself proof that a hand-written v2 producer is
        # cumulative.  Parse (but do not FNV-hash) the older entry lists and
        # skip them only when the newest verified set demonstrably contains
        # every one.  Otherwise the original per-record verifier below runs.
        collapse_metadata_valid = all(
            re.fullmatch(r'[0-9A-F]{16}', snapshot_fields(record)[1])
            for _, _, record in candidates)
        if len(candidates) > 1 and collapse_metadata_valid:
            _, _, newest_record = candidates[0]
            newest_path, newest_expected, newest_ref = snapshot_fields(newest_record)
            if (newest_path is not None and newest_ref not in seen_refs and
                    os.path.isfile(newest_path) and newest_expected):
                try:
                    # Hash and parse one byte buffer.  Reopening between those
                    # operations would permit a replacement-file TOCTOU.
                    newest_bytes = read_snapshot_bytes(newest_path)
                    if newest_expected != '%016X' % _fnv64_bytes(newest_bytes):
                        raise ValueError('snapshot signature mismatch')
                    newest_entries = entries_from_bytes(newest_bytes)
                    verified_probe[newest_ref] = newest_entries
                    cumulative = True
                    unverified_limit = min(
                        MAX_UNVERIFIED_SNAPSHOT_BYTES,
                        max(1024 * 1024, len(newest_bytes) * 2))
                    group_unverified_bytes = 0
                    for _, _, older_record in candidates[1:]:
                        older_path, _, _ = snapshot_fields(older_record)
                        if older_path is None or not os.path.isfile(older_path):
                            cumulative = False
                            break
                        remaining = (MAX_UNVERIFIED_TOTAL_BYTES -
                                     unverified_bytes_read)
                        if remaining <= 0:
                            cumulative = False
                            break
                        older_bytes = read_snapshot_bytes(
                            older_path, min(unverified_limit, remaining))
                        unverified_bytes_read += len(older_bytes)
                        group_unverified_bytes += len(older_bytes)
                        if not entries_from_bytes(older_bytes).issubset(newest_entries):
                            cumulative = False
                            break
                except Exception:
                    cumulative = False
                if cumulative:
                    seen_refs.add(newest_ref)
                    entries.update(newest_entries)
                    audit['verified_snapshots'] += 1
                    audit['unverified_superseded_records'] += len(candidates) - 1
                    audit['unverified_superseded_bytes'] += group_unverified_bytes
                    continue
        for _, lineno, record in candidates:
            snapshot, expected, ref = snapshot_fields(record)
            if snapshot is None:
                audit['invalid_records'] += 1
                continue
            if ref in seen_refs:
                continue
            seen_refs.add(ref)
            if not os.path.isfile(snapshot) or not expected:
                print(f'  warn: ignoring missing/mismatched snapshot on line '
                      f'{lineno}: {snapshot}')
                audit['invalid_records'] += 1
                continue
            try:
                snapshot_entries = verified_probe.pop(ref, None)
                if snapshot_entries is None:
                    snapshot_bytes = read_snapshot_bytes(snapshot)
                    if expected != '%016X' % _fnv64_bytes(snapshot_bytes):
                        raise ValueError('snapshot signature mismatch')
                    snapshot_entries = entries_from_bytes(snapshot_bytes)
                entries.update(snapshot_entries)
            except Exception as exc:
                print(f'  warn: ignoring unreadable/mismatched snapshot on line {lineno} '
                      f'({exc})')
                audit['invalid_records'] += 1
                continue
            audit['verified_snapshots'] += 1
    return entries, audit

def _crc_set(value):
    """Normalize legacy scalar CRC values and the current variant sets."""
    if value is None:
        return set()
    if isinstance(value, (set, frozenset, list, tuple)):
        return {int(crc) for crc in value if crc is not None}
    return {int(value)}


def recall(needed_entries, static_ent, static_ranges, needed_crc=None):
    """Return exact-entry, exact-candidate, and interval-potential metrics.

    ``static_ent`` and ``needed_crc`` map an entry address to every known code
    CRC variant at that VA.  Scalar values remain accepted for older callers.
    An R-interval hit is intentionally reported only as unverified potential:
    ranges from different content variants are merged for this diagnostic.
    """
    covered = set(static_ent)
    hit = needed_entries & covered
    miss = needed_entries - covered
    range_starts = [lo for lo, _ in static_ranges]
    def in_static_range(entry):
        i = bisect.bisect_right(range_starts, entry) - 1
        return i >= 0 and entry < static_ranges[i][1]
    range_hit = {entry for entry in needed_entries if in_static_range(entry)}
    range_miss = needed_entries - range_hit
    out = {'needed': len(needed_entries), 'covered_here': len(hit),
           'missed': len(miss), 'recall_entry': (len(hit)/len(needed_entries) if needed_entries else 0.0),
           'misses': sorted(miss),
           'covered_by_interval_potential': len(range_hit),
           'missed_interval_potential': len(range_miss),
           'recall_interval_potential': (len(range_hit)/len(needed_entries) if needed_entries else 0.0),
           'interval_potential_misses': sorted(range_miss),
           'interval_metric_semantics': 'unverified_address_containment_only',
           # Backward-compatible JSON aliases.  These do not imply a matching
           # content variant or a dispatchable CPS continuation.
           'covered_by_code_range': len(range_hit),
           'missed_code_range': len(range_miss),
           'recall_code_range': (len(range_hit)/len(needed_entries) if needed_entries else 0.0),
           'range_misses': sorted(range_miss)}
    if needed_crc is not None:
        needed_candidates = {
            (entry, crc)
            for entry in needed_entries
            for crc in _crc_set(needed_crc.get(entry))
        }
        static_candidates = {
            (entry, crc)
            for entry, variants in static_ent.items()
            for crc in _crc_set(variants)
        }
        crc_hits = needed_candidates & static_candidates
        crc_misses = needed_candidates - static_candidates
        crc_hit_addresses = {entry for entry, _ in crc_hits}
        out['needed_entry_crc'] = len(needed_candidates)
        out['covered_entry_crc'] = len(crc_hits)
        out['missed_entry_crc'] = len(crc_misses)
        out['recall_entry_crc'] = (
            len(crc_hits) / len(needed_candidates) if needed_candidates else 0.0)
        out['covered_entry_crc_addresses'] = len(crc_hit_addresses)
        out['recall_entry_crc_addresses'] = (
            len(crc_hit_addresses) / len(needed_entries) if needed_entries else 0.0)
        out['entry_crc_misses'] = sorted(crc_misses)
    return out

def group_by_region(addrs):
    g = defaultdict(list)
    for a in addrs: g[region_of(a)].append(a)
    return {r: sorted(v) for r, v in sorted(g.items())}


def candidate_miss_records(misses):
    """Stable JSON records for strict content-variant coverage gaps."""
    return [
        {'entry': f'0x{entry | 0x80000000:08X}',
         'code_crc': f'{crc:08X}'}
        for entry, crc in sorted(
            (norm(entry), crc & 0xFFFFFFFF) for entry, crc in misses)
    ]


def group_candidate_misses_by_region(misses):
    """Group strict ``(entry, code_crc)`` gaps without losing variants."""
    grouped = defaultdict(list)
    for entry, crc in sorted(misses):
        grouped[region_of(entry)].append((entry, crc))
    return {
        f'0x{region | 0x80000000:08X}': candidate_miss_records(values)
        for region, values in sorted(grouped.items())
    }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--static', required=True, help='static shard cache dir (.ranges)')
    ap.add_argument('--bios-dispatch',
                    help='generated base-BIOS dispatch C; adds already-native BIOS '
                         'entries/ranges to separate combined metrics')
    ap.add_argument('--vault', help='played/coverage vault cache dir (.ranges)')
    ap.add_argument('--captures', action='append',
                    help='runtime overlay_captures.json (repeat for multiple sessions)')
    ap.add_argument('--addendum', help='append-only runtime capture history (.jsonl)')
    ap.add_argument('--prior-report', help='roll forward a persisted live gap set')
    ap.add_argument('--assume-static-superset', action='store_true',
                    help='assert current static entries retain every prior static entry')
    ap.add_argument('--game', default='UNKNOWN')
    ap.add_argument('--out-md', required=True)
    ap.add_argument('--out-json', required=True)
    a = ap.parse_args()

    if a.prior_report and not a.assume_static_superset:
        ap.error('--prior-report requires the explicit --assume-static-superset assertion')

    static_ent, static_ranges, static_zero = parse_ranges_dir(
        a.static, with_audit=True)
    report = {'game': a.game, 'static_entries': len(static_ent), 'sources': {}}
    report['static_entry_crc_candidates'] = sum(
        len(_crc_set(variants)) for variants in static_ent.values())
    report['static_quarantined_zero_ranges'] = static_zero
    combined_ent = {
        entry: set(_crc_set(variants))
        for entry, variants in static_ent.items()
    }
    combined_ranges = static_ranges
    if a.bios_dispatch:
        try:
            bios_ent, bios_ranges = parse_bios_dispatch(a.bios_dispatch)
        except (OSError, ValueError) as exc:
            ap.error(f'cannot parse --bios-dispatch: {exc}')
        for entry in bios_ent:
            combined_ent.setdefault(entry, set()).add(None)
        combined_ranges = merge_intervals(static_ranges + bios_ranges)
        report['bios_dispatch'] = a.bios_dispatch
        report['bios_static_entries'] = len(bios_ent)
        report['bios_static_ranges'] = len(bios_ranges)

    md = []
    md.append(f'# AOT static-coverage recall — {a.game}\n')
    md.append('_How much of the played reference set did the play-free static '
              'extractor reproduce, and how much has unverified address overlap '
              'with a compiled static interval?_\n')
    md.append(f'- Static shard cache: `{a.static}`')
    md.append(f'- Static manifest entry addresses: **{len(static_ent)}**; '
              f'content-keyed entry+CRC candidates: '
              f'**{report["static_entry_crc_candidates"]}**\n')
    if static_zero:
        md.append(f'- Quarantined proven all-zero static ranges: '
                  f'**{len(static_zero)}** (excluded as data, not code)\n')
    if a.bios_dispatch:
        md.append(f'- Base BIOS native dispatch entries: **{len(bios_ent)}**; '
                  f'relocated kernel body ranges: **{len(bios_ranges)}**')
        md.append('- Combined metrics below count both the play-free overlay cache '
                  'and the separately generated, live-byte-guarded base BIOS.\n')

    if a.vault:
        vault_ent, _, vault_zero = parse_ranges_dir(a.vault, with_audit=True)
        report['vault_quarantined_zero_ranges'] = vault_zero
        r = recall(set(vault_ent), static_ent, static_ranges, vault_ent)
        report['sources']['vault'] = {
            k: v for k, v in r.items()
            if k not in ('misses', 'range_misses',
                         'interval_potential_misses', 'entry_crc_misses')}
        report['vault_misses'] = [f'0x{x|0x80000000:08X}' for x in r['misses']]
        report['vault_range_misses'] = [
            f'0x{x|0x80000000:08X}' for x in r['range_misses']]
        report['vault_interval_potential_misses'] = list(
            report['vault_range_misses'])
        report['vault_entry_crc_misses'] = candidate_miss_records(
            r['entry_crc_misses'])
        report['vault_entry_crc_misses_by_region'] = \
            group_candidate_misses_by_region(r['entry_crc_misses'])
        combined = None
        if a.bios_dispatch:
            combined = recall(set(vault_ent), combined_ent, combined_ranges)
            report['sources']['vault_combined_bios'] = {
                k: v for k, v in combined.items() if k not in ('misses', 'range_misses')}
            report['vault_combined_range_misses'] = [
                f'0x{x|0x80000000:08X}' for x in combined['range_misses']]
            report['vault_combined_interval_potential_misses'] = list(
                report['vault_combined_range_misses'])
        md.append('## vs played vault (most complete needed-set)\n')
        if vault_zero:
            md.append(f'- Quarantined proven all-zero played ranges: '
                      f'**{len(vault_zero)}** (excluded from the needed set)')
        md.append(f'- Manifest entries in the full-playthrough vault: **{r["needed"]}**')
        md.append(f'- Discovered by static: **{r["covered_here"]}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Contained in some compiled static interval (unverified '
                  f'potential): **{r["covered_by_interval_potential"]}** '
                  f'(**{r["recall_interval_potential"]*100:.1f}%**)')
        md.append('  - Interval containment alone does not prove native dispatch: '
                  'the owning content variant must pass its whole-function CRC guard, '
                  'and an interior PC must be a valid CPS continuation.')
        md.append(f'- Exact manifest candidates (entry+code_crc): '
                  f'**{r.get("covered_entry_crc",0)}/'
                  f'{r.get("needed_entry_crc",0)}** '
                  f'(**{r.get("recall_entry_crc",0)*100:.1f}%**) '
                  f'_(cg-version differences can lower this vs address-level recall)_')
        if r['entry_crc_misses']:
            md.append('#### Exact manifest-candidate gaps by region\n')
            for region, records in group_candidate_misses_by_region(
                    r['entry_crc_misses']).items():
                unique_addresses = len({record['entry'] for record in records})
                md.append(f'- `{region}`: **{len(records)}** missing '
                          f'candidate variant(s) at **{unique_addresses}** '
                          'entry address(es)')
            md.append('')
        md.append(f'- **MISSED exact entries: {r["missed"]}**')
        md.append(f'- **OUTSIDE ALL STATIC INTERVALS: '
                  f'{r["missed_interval_potential"]}** played entry PCs '
                  '(potential gaps; interval membership is not dispatch proof)\n')
        if combined:
            md.append('### Combined with base recompiled BIOS\n')
            md.append(f'- Exact entry addresses present in overlay/BIOS static '
                      f'dispatch sets: **{combined["covered_here"]}** '
                      f'(**{combined["recall_entry"]*100:.1f}%**)')
            md.append(f'- Contained in an overlay/BIOS static interval '
                      f'(unverified potential): '
                      f'**{combined["covered_by_interval_potential"]}** '
                      f'(**{combined["recall_interval_potential"]*100:.1f}%**)')
            md.append(f'- **OUTSIDE ALL COMBINED STATIC INTERVALS: '
                      f'{combined["missed_interval_potential"]}**\n')
            md.append('#### Addresses outside all combined static intervals\n')
            grouped_combined = group_by_region(combined['range_misses'])
            if not grouped_combined:
                md.append('- None.')
            else:
                for reg, addrs in grouped_combined.items():
                    md.append(f'- region `0x{reg|0x80000000:08X}`: {len(addrs)} misses')
                    md.append('  ```')
                    for i in range(0, len(addrs), 8):
                        md.append('  ' + ' '.join(
                            f'{x|0x80000000:08X}' for x in addrs[i:i+8]))
                    md.append('  ```')
            md.append('')
        md.append('### Addresses outside all static intervals, grouped by region\n')
        for reg, addrs in group_by_region(r['range_misses']).items():
            md.append(f'- region `0x{reg|0x80000000:08X}`: {len(addrs)} misses')
            md.append('  ```')
            for i in range(0, len(addrs), 8):
                md.append('  ' + ' '.join(f'{x|0x80000000:08X}' for x in addrs[i:i+8]))
            md.append('  ```')
        md.append('')

    live = set()
    live_sources = []
    capture_source_count = 0
    for captures_path in a.captures or []:
        if os.path.exists(captures_path):
            live.update(parse_captures_executed(captures_path))
            capture_source_count += 1
    if capture_source_count:
        live_sources.append(
            f'{capture_source_count} capture file'
            f'{"s" if capture_source_count != 1 else ""}')
    if a.addendum and os.path.exists(a.addendum):
        addendum_entries, addendum_audit = parse_addendum_entries(a.addendum)
        live.update(addendum_entries)
        live_sources.append('verified append-only history')
        report['live_addendum_audit'] = addendum_audit
    if live_sources:
        r = recall(live, static_ent, static_ranges)
        report['sources']['live_session'] = {
            k: v for k, v in r.items()
            if k not in ('misses', 'range_misses', 'interval_potential_misses')}
        report['live_misses'] = [f'0x{x|0x80000000:08X}' for x in r['misses']]
        report['live_range_misses'] = [
            f'0x{x|0x80000000:08X}' for x in r['range_misses']]
        report['live_interval_potential_misses'] = list(
            report['live_range_misses'])
        combined = None
        if a.bios_dispatch:
            combined = recall(live, combined_ent, combined_ranges)
            report['sources']['live_session_combined_bios'] = {
                k: v for k, v in combined.items() if k not in ('misses', 'range_misses')}
            report['live_combined_range_misses'] = [
                f'0x{x|0x80000000:08X}' for x in combined['range_misses']]
            report['live_combined_interval_potential_misses'] = list(
                report['live_combined_range_misses'])
        md.append('## vs live capture history\n')
        md.append(f'- Sources: **{" + ".join(live_sources)}**')
        if a.addendum and 'live_addendum_audit' in report:
            aa = report['live_addendum_audit']
            md.append(f'- FNV-verified immutable snapshots: '
                      f'**{aa["verified_snapshots"]}**; invalid records: '
                      f'**{aa["invalid_records"]}**')
            if aa.get('unverified_superseded_records'):
                unverified_mib = (aa.get('unverified_superseded_bytes', 0) /
                                  (1024 * 1024))
                md.append('- Superseded session snapshots **not FNV-verified** '
                          '(their parsed entry sets were subsets of a verified '
                          f'head): **{aa["unverified_superseded_records"]}** '
                          f'({unverified_mib:.1f} MiB parsed under bounds)')
        md.append(f'- Dispatch entries exercised: **{r["needed"]}**')
        md.append(f'- Discovered by static: **{r["covered_here"]}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Contained in some compiled static interval (unverified '
                  f'potential): **{r["covered_by_interval_potential"]}** '
                  f'(**{r["recall_interval_potential"]*100:.1f}%**)')
        md.append(f'- Outside all overlay static intervals (potential gaps): '
                  f'**{r["missed_interval_potential"]}**')
        if combined:
            md.append(f'- Including base BIOS static intervals (unverified '
                      f'potential): **{combined["covered_by_interval_potential"]}** '
                      f'(**{combined["recall_interval_potential"]*100:.1f}%**)')
            md.append(f'- Outside all combined static intervals (potential gaps): '
                      f'**{combined["missed_interval_potential"]}**')
        md.append(f'- Exact-entry misses (diagnostic; may be interior fragments): '
                  f'**{r["missed"]}**\n')
    if a.prior_report:
        if a.prior_report == '-':
            prior = json.load(sys.stdin)
        else:
            with open(a.prior_report, encoding='utf-8') as prior_in:
                prior = json.load(prior_in)
        prior_sources = prior.get('sources', {})
        if 'prior_live_session' in prior_sources:
            prior_live = prior_sources.get('prior_live_session')
            prior_misses = prior.get('prior_live_misses')
        else:
            prior_live = prior_sources.get('live_session')
            prior_misses = prior.get('live_misses')
        if not isinstance(prior_live, dict) or not isinstance(prior_misses, list):
            ap.error('--prior-report has no persisted live_session/live_misses')
        if len(static_ent) < int(prior.get('static_entries', 0)):
            ap.error('current static entry count is smaller than the prior report; '
                     'the asserted superset cannot hold')
        needed = int(prior_live.get('needed', 0))
        old_miss = {norm(int(x, 16)) for x in prior_misses}
        miss = old_miss - set(static_ent)
        range_starts = [lo for lo, _ in static_ranges]
        def in_static_range(entry):
            i = bisect.bisect_right(range_starts, entry) - 1
            return i >= 0 and entry < static_ranges[i][1]
        range_miss = {entry for entry in old_miss if not in_static_range(entry)}
        covered = needed - len(miss)
        r = {'needed': needed, 'covered_here': covered, 'missed': len(miss),
             'recall_entry': (covered/needed if needed else 0.0),
             'covered_by_interval_potential': needed - len(range_miss),
             'missed_interval_potential': len(range_miss),
             'recall_interval_potential': (
                 (needed - len(range_miss))/needed if needed else 0.0),
             'interval_metric_semantics': 'unverified_address_containment_only',
             'covered_by_code_range': needed - len(range_miss),
             'missed_code_range': len(range_miss),
             'recall_code_range': ((needed - len(range_miss))/needed if needed else 0.0)}
        r['provenance'] = ('rolled forward from the persisted live gap manifest; '
                           'caller asserted the current static entry set is a superset')
        prior_key = 'prior_live_session' if live_sources else 'live_session'
        prior_prefix = 'prior_live' if live_sources else 'live'
        report['sources'][prior_key] = r
        report[f'{prior_prefix}_misses'] = [
            f'0x{x|0x80000000:08X}' for x in sorted(miss)]
        report[f'{prior_prefix}_range_misses'] = [
            f'0x{x|0x80000000:08X}' for x in sorted(range_miss)]
        report[f'{prior_prefix}_interval_potential_misses'] = list(
            report[f'{prior_prefix}_range_misses'])
        combined = None
        if a.bios_dispatch:
            combined_miss = old_miss - set(combined_ent)
            combined_starts = [lo for lo, _ in combined_ranges]
            def in_combined_range(entry):
                i = bisect.bisect_right(combined_starts, entry) - 1
                return i >= 0 and entry < combined_ranges[i][1]
            combined_range_miss = {
                entry for entry in old_miss if not in_combined_range(entry)}
            combined_covered = needed - len(combined_miss)
            combined = {
                'needed': needed,
                'covered_here': combined_covered,
                'missed': len(combined_miss),
                'recall_entry': (combined_covered/needed if needed else 0.0),
                'covered_by_interval_potential': needed - len(combined_range_miss),
                'missed_interval_potential': len(combined_range_miss),
                'recall_interval_potential': (
                    (needed - len(combined_range_miss))/needed if needed else 0.0),
                'interval_metric_semantics': 'unverified_address_containment_only',
                'covered_by_code_range': needed - len(combined_range_miss),
                'missed_code_range': len(combined_range_miss),
                'recall_code_range': (
                    (needed - len(combined_range_miss))/needed if needed else 0.0),
                'provenance': r['provenance']}
            combined_key = ('prior_live_session_combined_bios' if live_sources
                            else 'live_session_combined_bios')
            report['sources'][combined_key] = combined
            report[f'{prior_prefix}_combined_range_misses'] = [
                f'0x{x|0x80000000:08X}' for x in sorted(combined_range_miss)]
            report[f'{prior_prefix}_combined_interval_potential_misses'] = list(
                report[f'{prior_prefix}_combined_range_misses'])
        heading = ('## vs pre-history persisted live-session gaps '
                   '(separate monotonic roll-forward)\n' if live_sources else
                   '## vs persisted live-session gaps (monotonic roll-forward)\n')
        md.append(heading)
        if live_sources:
            md.append('- This older needed set predates append-only history. It is '
                      'reported separately because the old report retained misses, '
                      'not enough addresses to compute an honest set union.\n')
        md.append(f'- Dispatch entries the persisted session exercised: **{needed}**')
        md.append(f'- Discovered by current static: **{covered}** '
                  f'(**{r["recall_entry"]*100:.1f}%** entry-level recall)')
        md.append(f'- Contained in a current static interval (unverified '
                  f'potential): **{r["covered_by_interval_potential"]}** '
                  f'(**{r["recall_interval_potential"]*100:.1f}%**)')
        md.append(f'- Outside all overlay static intervals (potential gaps): '
                  f'**{r["missed_interval_potential"]}**')
        if combined:
            md.append(f'- Including base BIOS static intervals (unverified '
                      f'potential): **{combined["covered_by_interval_potential"]}** '
                      f'(**{combined["recall_interval_potential"]*100:.1f}%**)')
            md.append(f'- Outside all combined static intervals (potential gaps): '
                      f'**{combined["missed_interval_potential"]}**')
        md.append(f'- Exact-entry misses (diagnostic; may be interior fragments): '
                  f'**{len(miss)}**')
        md.append('- Provenance: caller explicitly asserted that the current static '
                  'entry set retains every prior static entry.\n')

    for output_path in (a.out_md, a.out_json):
        output_dir = os.path.dirname(os.path.abspath(output_path))
        os.makedirs(output_dir, exist_ok=True)
    with open(a.out_md, 'w', encoding='utf-8', newline='\n') as out_md:
        out_md.write('\n'.join(md).rstrip('\n') + '\n')
    with open(a.out_json, 'w', encoding='utf-8', newline='\n') as out_json:
        json.dump(report, out_json, indent=1)
        out_json.write('\n')
    print(f'wrote {a.out_md}')
    print(f'wrote {a.out_json}')
    for src, m in report['sources'].items():
        print(f'  {src}: recall_entry={m.get("recall_entry",0)*100:.1f}% '
              f'({m.get("covered_here")}/{m.get("needed")}), '
              f'interval_potential={m.get("recall_interval_potential",0)*100:.1f}%, '
              f'missed={m.get("missed")}')

if __name__ == '__main__':
    main()
