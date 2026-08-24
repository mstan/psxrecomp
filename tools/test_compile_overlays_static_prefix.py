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


if __name__ == '__main__':
    unittest.main()
