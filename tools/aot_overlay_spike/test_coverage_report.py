#!/usr/bin/env python3
import os
import json
import sys
import tempfile
import unittest
import zlib
from unittest import mock

import coverage_report


class CoverageReportTests(unittest.TestCase):
    def test_bios_parser_is_scoped_to_named_tables(self):
        source = r'''
static const DispatchEntry dispatch_table[2] = {
    { 0x00000500u, func_00000500 },
    { 0xBFC00180u, func_BFC00180 },
};
static const unsigned unrelated[][3] = {
    { 0xDEADBEEFu, 0x00000001u, 0x1FFFFFFFu },
};
const PsxKernelBody psx_bios_kernel_bodies[2] = {
    { 0x00000500u, 0x00000500u, 0x00000510u },
    { 0x00000510u, 0x00000510u, 0x00000520u },
};
const PsxNativeStub psx_bios_native_stubs[1] = {
    { 0x000000A0u, 0x000000A0u, 0x000000B0u },
};
'''
        with tempfile.NamedTemporaryFile(
                mode='w', suffix='.c', delete=False, encoding='utf-8') as generated:
            generated.write(source)
            path = generated.name
        try:
            entries, ranges = coverage_report.parse_bios_dispatch(path)
        finally:
            os.unlink(path)
        self.assertEqual(entries, {0xA0, 0x500, 0x1FC00180})
        self.assertEqual(ranges, [(0xA0, 0xB0), (0x500, 0x520)])

    def test_interval_potential_is_distinct_from_exact_entry_recall(self):
        result = coverage_report.recall(
            {0x100, 0x104}, {0x100: {0x1234}}, [(0x100, 0x108)])
        self.assertEqual(result['covered_here'], 1)
        self.assertEqual(result['covered_by_interval_potential'], 2)
        self.assertEqual(result['covered_by_code_range'], 2)
        self.assertEqual(
            result['interval_metric_semantics'],
            'unverified_address_containment_only')

    def test_exact_candidate_recall_preserves_duplicate_va_crc_variants(self):
        result = coverage_report.recall(
            {0x100}, {0x100: {0xBBBB, 0xCCCC}}, [(0x100, 0x108)],
            {0x100: {0xAAAA, 0xBBBB}})
        self.assertEqual(result['covered_here'], 1)
        self.assertEqual(result['needed_entry_crc'], 2)
        self.assertEqual(result['covered_entry_crc'], 1)
        self.assertEqual(result['missed_entry_crc'], 1)
        self.assertEqual(result['recall_entry_crc'], 0.5)
        self.assertEqual(result['entry_crc_misses'], [(0x100, 0xAAAA)])

    def test_exact_candidate_misses_persist_variants_grouped_by_region(self):
        misses = [(0xA0001234, 0x1AAAAAAAA), (0x1234, 0xBBBBBBBB),
                  (0x2450, 0xCCCCCCCC)]
        self.assertEqual(coverage_report.candidate_miss_records(misses), [
            {'entry': '0x80001234', 'code_crc': 'AAAAAAAA'},
            {'entry': '0x80001234', 'code_crc': 'BBBBBBBB'},
            {'entry': '0x80002450', 'code_crc': 'CCCCCCCC'},
        ])
        self.assertEqual(
            coverage_report.group_candidate_misses_by_region(misses), {
                '0x80001000': [
                    {'entry': '0x80001234', 'code_crc': 'AAAAAAAA'},
                    {'entry': '0x80001234', 'code_crc': 'BBBBBBBB'},
                ],
                '0x80002000': [
                    {'entry': '0x80002450', 'code_crc': 'CCCCCCCC'},
                ],
            })

    def test_main_persists_and_summarizes_exact_candidate_gaps(self):
        with tempfile.TemporaryDirectory() as tmp:
            static = os.path.join(tmp, 'static')
            vault = os.path.join(tmp, 'vault')
            os.makedirs(static)
            os.makedirs(vault)
            with open(os.path.join(static, 'static.ranges'), 'w',
                      encoding='utf-8') as out:
                out.write('F 80001234 BBBBBBBB\nR 80001234 8\n')
            with open(os.path.join(vault, 'vault.ranges'), 'w',
                      encoding='utf-8') as out:
                out.write('F 80001234 AAAAAAAA\nR 80001234 8\n')
                out.write('F 80001234 BBBBBBBB\nR 80001234 8\n')
            out_md = os.path.join(tmp, 'gaps.md')
            out_json = os.path.join(tmp, 'gaps.json')
            argv = [
                'coverage_report.py', '--static', static, '--vault', vault,
                '--game', 'TEST', '--out-md', out_md, '--out-json', out_json,
            ]
            with mock.patch.object(sys, 'argv', argv):
                coverage_report.main()
            with open(out_json, encoding='utf-8') as source:
                report = json.load(source)
            with open(out_md, encoding='utf-8') as source:
                markdown = source.read()
        expected = [{'entry': '0x80001234', 'code_crc': 'AAAAAAAA'}]
        self.assertEqual(report['vault_entry_crc_misses'], expected)
        self.assertEqual(report['vault_entry_crc_misses_by_region'], {
            '0x80001000': expected,
        })
        self.assertIn('`0x80001000`: **1** missing candidate variant(s)',
                      markdown)

    def test_interval_containment_does_not_count_as_crc_candidate(self):
        result = coverage_report.recall(
            {0x100}, {0x100: {0xBBBB}}, [(0x100, 0x108)],
            {0x100: {0xAAAA}})
        self.assertEqual(result['covered_here'], 1)
        self.assertEqual(result['covered_by_interval_potential'], 1)
        self.assertEqual(result['covered_entry_crc'], 0)
        self.assertEqual(result['recall_entry_crc'], 0.0)

    def test_range_parser_retains_all_crc_variants_for_duplicate_va(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, 'a.ranges'), 'w', encoding='utf-8') as out:
                out.write('F 80000100 AAAAAAAA\nR 80000100 8\n')
            with open(os.path.join(tmp, 'b.ranges'), 'w', encoding='utf-8') as out:
                out.write('F 80000100 BBBBBBBB\nR 80000100 C\n')
            entries, ranges = coverage_report.parse_ranges_dir(tmp)
        self.assertEqual(entries, {0x100: {0xAAAAAAAA, 0xBBBBBBBB}})
        self.assertEqual(ranges, [(0x100, 0x10C)])

    def test_all_zero_compiled_range_is_quarantined(self):
        with tempfile.TemporaryDirectory() as tmp:
            zero_size = 0x184
            zero_crc = zlib.crc32(bytes(zero_size)) & 0xFFFFFFFF
            with open(os.path.join(tmp, 'mixed.ranges'), 'w', encoding='utf-8') as out:
                out.write('# psxrecomp overlay code-range manifest v2\n')
                out.write('F 8000EE7C %08X\n' % zero_crc)
                out.write('R 8000EE7C %X\n' % zero_size)
                out.write('F 80000100 12345678\n')
                out.write('R 80000100 10\n')
            entries, ranges, audit = coverage_report.parse_ranges_dir(
                tmp, with_audit=True)
        self.assertEqual(entries, {0x100: {0x12345678}})
        self.assertEqual(ranges, [(0x100, 0x110)])
        self.assertEqual(audit, [{
            'entry': '0x8000EE7C', 'code_crc': '%08X' % zero_crc,
            'size': zero_size}])

    def test_addendum_unions_v1_and_verified_v2(self):
        with tempfile.TemporaryDirectory() as tmp:
            snapshot = os.path.join(tmp, 'immutable.json')
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(snapshot, 'w', encoding='utf-8') as out:
                json.dump([{'dispatch_entry_pcs': ['0x80000200']}], out)
            signature = '%016X' % coverage_report._fnv64_file(snapshot)
            records = [
                {'schema': 'psxrecomp overlay capture addendum v1',
                 'captures': [{'function_entry_pcs': ['0x80000100']}]},
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'snapshot': snapshot, 'fnv64': signature},
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'snapshot': snapshot, 'fnv64': signature},
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'snapshot': snapshot, 'fnv64': '0000000000000000'},
            ]
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for record in records:
                    out.write(json.dumps(record) + '\n')
                out.write('{"schema":"torn')
            entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200})
        self.assertEqual(audit, {
            'v1_records': 1, 'v2_records': 3, 'invalid_records': 2,
            'duplicate_refs': 1, 'verified_snapshots': 1,
            'unverified_superseded_records': 0,
            'unverified_superseded_bytes': 0})

    def test_addendum_uses_newest_cumulative_snapshot_per_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            snapshots = []
            captures = [
                [{'dispatch_entry_pcs': ['0x80000100']}],
                [{'dispatch_entry_pcs': ['0x80000100', '0x80000200']}],
            ]
            for index, body in enumerate(captures, 1):
                snapshot = os.path.join(tmp, f'immutable-{index}.json')
                with open(snapshot, 'w', encoding='utf-8') as out:
                    json.dump(body, out)
                snapshots.append(snapshot)
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for sequence, snapshot in enumerate(snapshots, 1):
                    out.write(json.dumps({
                        'schema': 'psxrecomp overlay capture addendum v2',
                        'game': 'TEST', 'session': 'session-1',
                        'sequence': sequence, 'snapshot': snapshot,
                        'fnv64': '%016X' % coverage_report._fnv64_file(snapshot),
                    }) + '\n')
            entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200})
        self.assertEqual(audit['v2_records'], 2)
        self.assertEqual(audit['verified_snapshots'], 1)
        self.assertEqual(audit['unverified_superseded_records'], 1)

    def test_addendum_falls_back_when_newest_snapshot_is_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            older = os.path.join(tmp, 'older.json')
            newer = os.path.join(tmp, 'newer.json')
            with open(older, 'w', encoding='utf-8') as out:
                json.dump([{'function_entry_pcs': ['0x80000100']}], out)
            with open(newer, 'w', encoding='utf-8') as out:
                json.dump([{'function_entry_pcs': ['0x80000200']}], out)
            addendum = os.path.join(tmp, 'history.jsonl')
            records = [
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'game': 'TEST', 'session': 'session-1', 'sequence': 1,
                 'snapshot': older,
                 'fnv64': '%016X' % coverage_report._fnv64_file(older)},
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'game': 'TEST', 'session': 'session-1', 'sequence': 2,
                 'snapshot': newer, 'fnv64': '0000000000000000'},
            ]
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for record in records:
                    out.write(json.dumps(record) + '\n')
            entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100})
        self.assertEqual(audit['invalid_records'], 1)
        self.assertEqual(audit['verified_snapshots'], 1)
        self.assertEqual(audit['unverified_superseded_records'], 0)

    def test_addendum_does_not_collapse_nonmonotonic_session_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            records = []
            for sequence, address in ((2, '0x80000200'), (1, '0x80000100')):
                snapshot = os.path.join(tmp, f'immutable-{sequence}.json')
                with open(snapshot, 'w', encoding='utf-8') as out:
                    json.dump([{'dispatch_entry_pcs': [address]}], out)
                records.append({
                    'schema': 'psxrecomp overlay capture addendum v2',
                    'game': 'TEST', 'session': 'session-1',
                    'sequence': sequence, 'snapshot': snapshot,
                    'fnv64': '%016X' % coverage_report._fnv64_file(snapshot),
                })
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for record in records:
                    out.write(json.dumps(record) + '\n')
            entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200})
        self.assertEqual(audit['verified_snapshots'], 2)
        self.assertEqual(audit['unverified_superseded_records'], 0)

    def test_addendum_does_not_collapse_non_cumulative_session_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            records = []
            for sequence, address in ((1, '0x80000100'), (2, '0x80000200')):
                snapshot = os.path.join(tmp, f'immutable-{sequence}.json')
                with open(snapshot, 'w', encoding='utf-8') as out:
                    json.dump([{'dispatch_entry_pcs': [address]}], out)
                records.append({
                    'schema': 'psxrecomp overlay capture addendum v2',
                    'game': 'TEST', 'session': 'session-1',
                    'sequence': sequence, 'snapshot': snapshot,
                    'fnv64': '%016X' % coverage_report._fnv64_file(snapshot),
                })
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for record in records:
                    out.write(json.dumps(record) + '\n')
            entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200})
        self.assertEqual(audit['verified_snapshots'], 2)
        self.assertEqual(audit['unverified_superseded_records'], 0)

    def test_tampered_superseded_hash_cannot_change_score(self):
        with tempfile.TemporaryDirectory() as tmp:
            older = os.path.join(tmp, 'older.json')
            newer = os.path.join(tmp, 'newer.json')
            with open(older, 'w', encoding='utf-8') as out:
                json.dump([{'dispatch_entry_pcs': ['0x80000100']}], out)
            stale_signature = '%016X' % coverage_report._fnv64_file(older)
            # Change the bytes while preserving an entry set contained by the
            # verified head. The old file must not contribute to the score.
            with open(older, 'a', encoding='utf-8') as out:
                out.write(' ')
            with open(newer, 'w', encoding='utf-8') as out:
                json.dump([{'dispatch_entry_pcs': [
                    '0x80000100', '0x80000200']}], out)
            records = [
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'game': 'TEST', 'session': 'session-1', 'sequence': 1,
                 'snapshot': older, 'fnv64': stale_signature},
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'game': 'TEST', 'session': 'session-1', 'sequence': 2,
                 'snapshot': newer,
                 'fnv64': '%016X' % coverage_report._fnv64_file(newer)},
            ]
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for record in records:
                    out.write(json.dumps(record) + '\n')
            entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200})
        self.assertEqual(audit['verified_snapshots'], 1)
        self.assertEqual(audit['invalid_records'], 0)
        self.assertEqual(audit['unverified_superseded_records'], 1)

    def test_bad_older_fnv_metadata_forces_invalid_record_audit(self):
        for label, bad_fnv in (('missing', None), ('garbage', 'not-a-hash')):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as tmp:
                older = os.path.join(tmp, 'older.json')
                newer = os.path.join(tmp, 'newer.json')
                with open(older, 'w', encoding='utf-8') as out:
                    json.dump([{'dispatch_entry_pcs': ['0x80000100']}], out)
                with open(newer, 'w', encoding='utf-8') as out:
                    json.dump([{'dispatch_entry_pcs': [
                        '0x80000100', '0x80000200']}], out)
                older_record = {
                    'schema': 'psxrecomp overlay capture addendum v2',
                    'game': 'TEST', 'session': 'session-1', 'sequence': 1,
                    'snapshot': older,
                }
                if bad_fnv is not None:
                    older_record['fnv64'] = bad_fnv
                records = [
                    older_record,
                    {'schema': 'psxrecomp overlay capture addendum v2',
                     'game': 'TEST', 'session': 'session-1', 'sequence': 2,
                     'snapshot': newer,
                     'fnv64': '%016X' % coverage_report._fnv64_file(newer)},
                ]
                addendum = os.path.join(tmp, 'history.jsonl')
                with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                    for record in records:
                        out.write(json.dumps(record) + '\n')
                entries, audit = coverage_report.parse_addendum_entries(addendum)
            self.assertEqual(entries, {0x100, 0x200})
            self.assertEqual(audit['invalid_records'], 1)
            self.assertEqual(audit['verified_snapshots'], 1)
            self.assertEqual(audit['unverified_superseded_records'], 0)

    def test_oversized_unverified_snapshot_forces_full_verification(self):
        with tempfile.TemporaryDirectory() as tmp:
            older = os.path.join(tmp, 'older.json')
            newer = os.path.join(tmp, 'newer.json')
            with open(older, 'w', encoding='utf-8') as out:
                json.dump([{'dispatch_entry_pcs': ['0x80000100']}], out)
                out.write(' ' * 64)
            with open(newer, 'w', encoding='utf-8') as out:
                json.dump([{'dispatch_entry_pcs': [
                    '0x80000100', '0x80000200']}], out)
            records = [
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'game': 'TEST', 'session': 'session-1', 'sequence': 1,
                 'snapshot': older,
                 'fnv64': '%016X' % coverage_report._fnv64_file(older)},
                {'schema': 'psxrecomp overlay capture addendum v2',
                 'game': 'TEST', 'session': 'session-1', 'sequence': 2,
                 'snapshot': newer,
                 'fnv64': '%016X' % coverage_report._fnv64_file(newer)},
            ]
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for record in records:
                    out.write(json.dumps(record) + '\n')
            with mock.patch.object(
                    coverage_report, 'MAX_UNVERIFIED_SNAPSHOT_BYTES', 32):
                entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200})
        self.assertEqual(audit['verified_snapshots'], 2)
        self.assertEqual(audit['unverified_superseded_records'], 0)

    def test_aggregate_unverified_size_bound_forces_full_verification(self):
        with tempfile.TemporaryDirectory() as tmp:
            snapshots = []
            bodies = [
                [{'dispatch_entry_pcs': ['0x80000100']}],
                [{'dispatch_entry_pcs': ['0x80000100', '0x80000200']}],
                [{'dispatch_entry_pcs': [
                    '0x80000100', '0x80000200', '0x80000300']}],
            ]
            for sequence, body in enumerate(bodies, 1):
                snapshot = os.path.join(tmp, f'immutable-{sequence}.json')
                with open(snapshot, 'w', encoding='utf-8') as out:
                    json.dump(body, out)
                snapshots.append(snapshot)
            addendum = os.path.join(tmp, 'history.jsonl')
            with open(addendum, 'w', encoding='utf-8', newline='\n') as out:
                for sequence, snapshot in enumerate(snapshots, 1):
                    out.write(json.dumps({
                        'schema': 'psxrecomp overlay capture addendum v2',
                        'game': 'TEST', 'session': 'session-1',
                        'sequence': sequence, 'snapshot': snapshot,
                        'fnv64': '%016X' % coverage_report._fnv64_file(snapshot),
                    }) + '\n')
            with mock.patch.object(
                    coverage_report, 'MAX_UNVERIFIED_TOTAL_BYTES', 1):
                entries, audit = coverage_report.parse_addendum_entries(addendum)
        self.assertEqual(entries, {0x100, 0x200, 0x300})
        self.assertEqual(audit['verified_snapshots'], 3)
        self.assertEqual(audit['unverified_superseded_records'], 0)


if __name__ == '__main__':
    unittest.main()
