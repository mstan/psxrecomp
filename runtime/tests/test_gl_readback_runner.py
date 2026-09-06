import unittest
from run_gl_readback_region import probe_result_ok


class ProbeResultTest(unittest.TestCase):
    def test_success(self):
        self.assertTrue(probe_result_ok(0, "driver=GL\nchecks=67 failures=0\n", ""))

    def test_exact_negative(self):
        self.assertTrue(probe_result_ok(1, "checks=67 failures=1\n",
                                       "FAIL single-pixel bounded transfer\n", True))

    def test_negative_rejects_other_counts(self):
        for count in (0, 10, 11, 100, 199):
            with self.subTest(count=count):
                self.assertFalse(probe_result_ok(1, f"checks=67 failures={count}\n",
                                                "FAIL single-pixel bounded transfer\n", True))

    def test_negative_requires_expected_failure(self):
        for error in ("", "FAIL GL error\n", "FAIL single-pixel bounded transfer\nFAIL other\n"):
            self.assertFalse(probe_result_ok(1, "checks=67 failures=1\n", error, True))

    def test_rejects_missing_or_malformed_summary(self):
        for text in ("", "checks=67 failures=1 extra", "checks=0 failures=0",
                     "checks=67 failures=0\ntrailing text", "checks=67 failures=10"):
            self.assertFalse(probe_result_ok(0, text, ""))

    def test_rejects_exit_and_error_mismatch(self):
        self.assertFalse(probe_result_ok(1, "checks=67 failures=0", ""))
        self.assertFalse(probe_result_ok(0, "checks=67 failures=0", "FAIL other"))
        self.assertFalse(probe_result_ok(0, "checks=67 failures=1",
                                        "FAIL single-pixel bounded transfer", True))


if __name__ == "__main__":
    unittest.main()
