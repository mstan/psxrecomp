import argparse
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays


class StaticSymbolPrefixTests(unittest.TestCase):
    def setUp(self):
        self.variants = [{
            'addr': 0x807814D0,
            'symbol': 'ov_highram_recipe_func_807814D0',
            'crc': 0x12345678,
            'ranges': [(0x007814D0, 0x20)],
        }]

    def test_default_dispatch_names_remain_compatible(self):
        output = compile_overlays.generate_overlay_dispatch(self.variants)

        self.assertIn('int psx_overlay_dispatch(CPUState *cpu, uint32_t addr)',
                      output)
        self.assertIn('void psx_overlay_static_get_stats(', output)
        self.assertIn('psx_ov_static_ranges_00000', output)
        self.assertNotIn('psx_overlay_dispatch_', output)
        self.assertEqual(
            compile_overlays.static_recipe_namespace('recipe'), 'ov_recipe')

    def test_highram_prefix_matches_static_bundle_convention(self):
        output = compile_overlays.generate_overlay_dispatch(
            self.variants, 'highram')

        self.assertIn(
            'int psx_overlay_dispatch_highram(CPUState *cpu, uint32_t addr)',
            output)
        self.assertIn('void psx_overlay_highram_get_stats(', output)
        self.assertIn('psx_ov_highram_static_ranges_00000', output)
        self.assertNotIn('psx_ov_static_ranges_', output)
        self.assertEqual(
            compile_overlays.static_recipe_namespace('recipe', 'highram'),
            'ov_highram_recipe')

    def test_prefix_validation_is_a_conservative_c_identifier(self):
        for value in ('highram', '_private', 'highram2'):
            self.assertEqual(
                compile_overlays.parse_static_symbol_prefix(value), value)
        for value in ('', '2highram', 'high-ram', 'high ram', 'highram;'):
            with self.subTest(value=value), self.assertRaises(
                    argparse.ArgumentTypeError):
                compile_overlays.parse_static_symbol_prefix(value)

    def test_resume_cases_are_inserted_in_one_host_update(self):
        src = ('void ov_func_80780000(CPUState* cpu)\n{\n'
               '    debug_server_log_call_entry(cpu);\n'
               'block_80780004:\n    cpu->pc = 0;\n'
               'block_80780008:\n    return;\n}\n')
        output, ok = compile_overlays.add_cps_resume_cases(
            src, 'ov_func_80780000', 0x80780000,
            [0x80780008, 0x80780004])

        self.assertTrue(ok)
        self.assertEqual(output.count('if (cpu->pc != 0u)'), 1)
        self.assertIn(
            'case 0x80780004u: goto block_80780004;', output)
        self.assertIn(
            'case 0x80780008u: goto block_80780008;', output)

    def test_multiple_hosts_are_rebuilt_in_one_shard_edit(self):
        src = ('void ov_a(CPUState* cpu)\n{\nblock_80780004:\n return;\n}\n'
               'void ov_b(CPUState* cpu)\n{\nblock_80780104:\n return;\n}\n')
        output, admitted = compile_overlays.add_cps_resume_cases_by_host(
            src, [('ov_a', 0x80780000, [0x80780004]),
                  ('ov_b', 0x80780100, [0x80780104])])

        self.assertEqual(admitted, {0x80780004, 0x80780104})
        self.assertEqual(output.count('if (cpu->pc != 0u)'), 2)
        self.assertIn('case 0x80780004u: goto block_80780004;', output)
        self.assertIn('case 0x80780104u: goto block_80780104;', output)


if __name__ == '__main__':
    unittest.main()
