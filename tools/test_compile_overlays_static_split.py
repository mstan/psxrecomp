"""Static-mode output split + worker contract for compile_overlays.py.

    python tools/test_compile_overlays_static_split.py
"""
import os
import pickle
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays  # noqa: E402


def part(ns, body, symbols):
    return {
        'src': f'#include "psx_runtime.h"\n{body}\n',
        'namespace': ns,
        'variants': [{'addr': 0x80100000 + i * 4, 'symbol': s, 'crc': 1,
                      'ranges': ((0x00100000, 8),)} for i, s in enumerate(symbols)],
        'func_addrs': set(), 'symbols': {}, 'ids_by_addr': {},
        'continuation_owners': {},
    }


class StaticSplitTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = os.path.join(self.tmp.name, 'overlays_static.c')

    def tearDown(self):
        self.tmp.cleanup()

    def test_split_writes_dispatcher_prototypes_and_parts(self):
        parts = [part('ov_a', 'void ov_a_func_80100000(CPUState *cpu) {}',
                      ['ov_a_func_80100000']),
                 part('ov_b', 'void ov_b_func_80100004(CPUState *cpu) {}',
                      ['ov_b_func_80100004'])]
        variants = [v for p in parts for v in p['variants']]
        written = compile_overlays.write_static_outputs(
            self.out, parts, variants, '/* dispatch */\n')
        self.assertEqual([os.path.basename(w) for w in written],
                         ['overlays_static.c', 'overlays_static_0000.c',
                          'overlays_static_0001.c'])
        main = open(self.out, encoding='utf-8').read()
        self.assertIn('#include "psx_runtime.h"', main)
        self.assertIn('void ov_a_func_80100000(CPUState *cpu);', main)
        self.assertIn('void ov_b_func_80100004(CPUState *cpu);', main)
        self.assertIn('/* dispatch */', main)
        self.assertNotIn('ov_a_func_80100000(CPUState *cpu) {}', main)
        p0 = open(written[1], encoding='utf-8').read()
        self.assertIn('ov_a_func_80100000(CPUState *cpu) {}', p0)
        self.assertIn('#include "psx_runtime.h"', p0)
        self.assertNotIn('ov_b_func', p0)

    def test_unchanged_part_keeps_mtime_and_stale_parts_are_removed(self):
        parts = [part('ov_a', 'int a;', ['ov_a_func_80100000']),
                 part('ov_b', 'int b;', ['ov_b_func_80100004']),
                 part('ov_c', 'int c;', ['ov_c_func_80100008'])]
        variants = [v for p in parts for v in p['variants']]
        first = compile_overlays.write_static_outputs(
            self.out, parts, variants, '/* d */\n')
        self.assertEqual(len(first), 4)
        old_mtime = os.stat(first[1]).st_mtime_ns
        os.utime(first[1], ns=(old_mtime - 5_000_000_000,
                               old_mtime - 5_000_000_000))
        old_mtime = os.stat(first[1]).st_mtime_ns
        time.sleep(0.01)
        # Drop the last part; part 0 unchanged, part 1 changed.
        parts[1] = part('ov_b', 'int b2;', ['ov_b_func_80100004'])
        parts = parts[:2]
        variants = [v for p in parts for v in p['variants']]
        second = compile_overlays.write_static_outputs(
            self.out, parts, variants, '/* d */\n')
        self.assertEqual(len(second), 3)
        self.assertEqual(os.stat(second[1]).st_mtime_ns, old_mtime,
                         'unchanged part must not be rewritten')
        self.assertIn('int b2;', open(second[2], encoding='utf-8').read())
        self.assertFalse(os.path.exists(first[3]), 'stale part must be deleted')
        self.assertEqual(sorted(os.listdir(self.tmp.name)),
                         ['overlays_static.c', 'overlays_static_0000.c',
                          'overlays_static_0001.c'])

    def test_single_file_mode_is_monolithic_and_cleans_parts(self):
        parts = [part('ov_a', 'int a;', ['ov_a_func_80100000'])]
        variants = parts[0]['variants']
        compile_overlays.write_static_outputs(self.out, parts, variants, '/* d */\n')
        self.assertTrue(os.path.exists(os.path.join(self.tmp.name,
                                                    'overlays_static_0000.c')))
        written = compile_overlays.write_static_outputs(
            self.out, parts, variants, '/* d */\n', single_file=True)
        self.assertEqual(written, [self.out])
        self.assertEqual(os.listdir(self.tmp.name), ['overlays_static.c'])
        main = open(self.out, encoding='utf-8').read()
        self.assertIn('int a;', main)
        self.assertIn('/* d */', main)
        self.assertNotIn('(CPUState *cpu);', main)

    def test_static_part_paths_only_matches_numbered_siblings(self):
        for name in ('overlays_static_0000.c', 'overlays_static_0001.c',
                     'overlays_static.c', 'overlays_static_x.c',
                     'other_0000.c'):
            open(os.path.join(self.tmp.name, name), 'w').close()
        found = [os.path.basename(p)
                 for p in compile_overlays.static_part_paths(self.out)]
        self.assertEqual(found, ['overlays_static_0000.c',
                                 'overlays_static_0001.c'])


class StaticWorkerContractTests(unittest.TestCase):
    def test_worker_is_picklable_for_a_process_pool(self):
        # ProcessPoolExecutor pickles the callable by qualified name; a nested
        # closure would fail here. This is the guard for that regression.
        fn = pickle.loads(pickle.dumps(compile_overlays.static_capture_job))
        self.assertIs(fn, compile_overlays.static_capture_job)

    def test_data_only_capture_reports_skip_with_captured_log(self):
        import argparse
        import base64
        cap = {
            'schema': 'psxrecomp overlay capture v2',
            'load_addr': '0x80100000',
            'size': 16,
            'bytes_b64': base64.b64encode(b'\0' * 16).decode('ascii'),
            'executed_pcs': [], 'dispatch_entry_pcs': [],
            'function_entry_pcs': [],
        }
        args = argparse.Namespace(recompiler='unused', game_toml='unused',
                                  cps=False, project_root=None)
        res = compile_overlays.static_capture_job(cap, args, {}, set(),
                                                  'overlays_static.c')
        self.assertEqual(res['outcome'], 'skip')
        self.assertIsNone(res['part'])
        self.assertIn('SKIP: no walk-root seeds', res['log'])
        self.assertEqual(res['requested_entries'], set())


HOST_SRC = """#include "psx_runtime.h"
void ov_x_func_80100000(CPUState* cpu);
void ov_x_func_80100000(CPUState* cpu)
{
    debug_server_log_call_entry(0x80100000u);
block_80100010:
    cpu->pc = 1;
block_80100020:
    cpu->pc = 2;
block_80100030:
    cpu->pc = 3;
}
void ov_x_func_80100100(CPUState* cpu)
{
    if (cpu->pc != 0u) {
        uint32_t _cont = cpu->pc; cpu->pc = 0;
        switch (_cont) {
            case 0x80100100u: break;
            default: cpu->pc = _cont; psx_native_bad_entry(cpu, 0x80100100u, _cont); return;
        }
    }
    debug_server_log_call_entry(0x80100100u);
block_80100110:
    cpu->pc = 4;
block_80100120:
    cpu->pc = 5;
}
"""


class ResumeCaseBatchTests(unittest.TestCase):
    """add_cps_resume_cases must be byte-identical to looping
    add_cps_resume_case entry by entry (the pre-batch implementation)."""

    def _sequential(self, src, requests):
        ok = {}
        for host_symbol, host, entries in requests:
            for entry in entries:
                src, good = compile_overlays.add_cps_resume_case(
                    src, host_symbol, host, entry)
                if good:
                    ok.setdefault(host_symbol, set()).add(entry)
        return src, ok

    def test_batch_matches_sequential_fresh_switch_and_existing_switch(self):
        requests = [
            ('ov_x_func_80100000', 0x80100000,
             [0x80100010, 0x80100020, 0x80100030, 0x80100999]),   # no such block
            ('ov_x_func_80100100', 0x80100100, [0x80100110, 0x80100120]),
            ('ov_x_func_missing', 0x80100200, [0x80100210]),       # no definition
        ]
        seq_src, seq_ok = self._sequential(HOST_SRC, requests)
        bat_src, bat_ok = compile_overlays.add_cps_resume_cases(HOST_SRC, requests)
        self.assertEqual(bat_src, seq_src)
        self.assertEqual({k: v for k, v in bat_ok.items() if v}, seq_ok)
        self.assertIn('case 0x80100010u: goto block_80100010;', bat_src)
        self.assertIn('case 0x80100120u: goto block_80100120;', bat_src)
        self.assertNotIn('80100999', bat_src)
        # Re-applying is a no-op (arms already present).
        again_src, again_ok = compile_overlays.add_cps_resume_cases(bat_src, requests)
        self.assertEqual(again_src, bat_src)
        self.assertEqual(again_ok['ov_x_func_80100000'],
                         {0x80100010, 0x80100020, 0x80100030})

    def test_batch_edits_are_confined_to_each_host_segment(self):
        requests = [('ov_x_func_80100100', 0x80100100, [0x80100110])]
        bat_src, _ = compile_overlays.add_cps_resume_cases(HOST_SRC, requests)
        marker = 'void ov_x_func_80100100(CPUState* cpu)\n{'
        head = bat_src[:bat_src.index(marker)]
        self.assertEqual(head, HOST_SRC[:HOST_SRC.index(marker)])


if __name__ == '__main__':
    unittest.main()
