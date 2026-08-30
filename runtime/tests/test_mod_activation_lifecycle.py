#!/usr/bin/env python3
"""Guard the commit -> activate -> boot ordering for every runtime session."""

import re
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src/main.cpp"


class ModActivationLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_raw_activation_is_centralized(self) -> None:
        calls = re.findall(
            r"(?m)^\s*mod_runtime_activate_plugins\(\);$", self.source
        )
        self.assertEqual(len(calls), 1)

    def test_initial_and_rematch_plans_activate_before_boot(self) -> None:
        calls = [
            match.start()
            for match in re.finditer(
                r"(?m)^\s*activate_committed_mod_plan\(player_mode,", self.source
            )
        ]
        self.assertEqual(len(calls), 2)

        initial_commit = self.source.index("cannot apply netplay mods:")
        session_reboot = self.source.index("session_reboot:")
        self.assertLess(initial_commit, calls[0])
        self.assertLess(calls[0], session_reboot)

        rematch_commit = self.source.index("cannot relaunch with selected")
        rematch_reboot = self.source.index("goto session_reboot;", rematch_commit)
        self.assertLess(rematch_commit, calls[1])
        self.assertLess(calls[1], rematch_reboot)


if __name__ == "__main__":
    unittest.main()
