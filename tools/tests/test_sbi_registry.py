from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from generate_sbi_registry import render


class RegistryTests(unittest.TestCase):
    def test_native_registry_matches_intake_metadata(self):
        self.assertEqual((ROOT / "runtime/include/sbi_registry.h").read_text(), render())


if __name__ == "__main__":
    unittest.main()
