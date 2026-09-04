#!/usr/bin/env python3
"""Unit tests for pad_trace's classification of input doubling."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pad_trace as pt


NONE = 0xFFFF


def sample(t, frame, buttons=NONE, sticks=None, analog=False):
    return {"t": t, "frame": frame, "buttons": buttons,
            "sticks": sticks or [0x80, 0x80, 0x80, 0x80], "analog": analog}


def hold(name):
    return NONE & ~pt.BUTTONS[name]


def stream(specs, hz=200.0, start_frame=0):
    """specs = list of (n_samples, buttons, sticks, analog)."""
    out, t, frame = [], 0.0, start_frame
    for n, buttons, sticks, analog in specs:
        for _ in range(n):
            out.append(sample(t, int(frame), buttons, list(sticks), analog))
            t += 1.0 / hz
            frame += 60.0 / hz
    return out


class PressedTest(unittest.TestCase):
    def test_active_low(self):
        self.assertFalse(pt.pressed(NONE, "up"))
        self.assertTrue(pt.pressed(hold("up"), "up"))

    def test_other_bits_unaffected(self):
        self.assertFalse(pt.pressed(hold("up"), "down"))


class StickTest(unittest.TestCase):
    def test_centre_is_not_deflected(self):
        self.assertFalse(pt.stick_deflected([0x80, 0x80, 0x80, 0x80]))

    def test_full_deflection_detected(self):
        self.assertTrue(pt.stick_deflected([0x00, 0x80, 0x80, 0x80]))
        self.assertTrue(pt.stick_deflected([0x80, 0xFF, 0x80, 0x80]))

    def test_right_stick_ignored(self):
        """Only the left stick drives movement/menus here."""
        self.assertFalse(pt.stick_deflected([0x80, 0x80, 0x00, 0xFF]))

    def test_inside_deadzone_ignored(self):
        self.assertFalse(pt.stick_deflected([0x80 + 0x10, 0x80, 0x80, 0x80]))


class FindRunsTest(unittest.TestCase):
    def test_single_run(self):
        s = stream([(5, NONE, [0x80] * 4, False),
                    (6, hold("up"), [0x80] * 4, False),
                    (5, NONE, [0x80] * 4, False)])
        self.assertEqual(pt.find_runs(s, "up"), [(5, 10)])

    def test_two_runs(self):
        s = stream([(2, NONE, [0x80] * 4, False),
                    (3, hold("up"), [0x80] * 4, False),
                    (2, NONE, [0x80] * 4, False),
                    (3, hold("up"), [0x80] * 4, False)])
        self.assertEqual(pt.find_runs(s, "up"), [(2, 4), (7, 9)])

    def test_run_open_at_end_is_closed(self):
        s = stream([(2, NONE, [0x80] * 4, False),
                    (3, hold("up"), [0x80] * 4, False)])
        self.assertEqual(pt.find_runs(s, "up"), [(2, 4)])

    def test_no_press(self):
        s = stream([(4, NONE, [0x80] * 4, False)])
        self.assertEqual(pt.find_runs(s, "up"), [])


class AnalyzeTest(unittest.TestCase):
    def kinds(self, findings, button=None):
        return {f["kind"] for f in findings
                if button is None or f["button"] == button}

    def test_clean_tap_reports_nothing(self):
        """One tap, stick centred, digital pad: no mechanism to report."""
        s = stream([(20, NONE, [0x80] * 4, False),
                    (20, hold("up"), [0x80] * 4, False),
                    (20, NONE, [0x80] * 4, False)])
        runs, findings = pt.analyze(s)
        self.assertEqual(len(runs), 1)
        self.assertEqual(findings, [])

    def test_double_press_detected(self):
        s = stream([(10, NONE, [0x80] * 4, False),
                    (5, hold("up"), [0x80] * 4, False),
                    (5, NONE, [0x80] * 4, False),
                    (5, hold("up"), [0x80] * 4, False),
                    (10, NONE, [0x80] * 4, False)])
        _, findings = pt.analyze(s)
        self.assertIn("double_press", self.kinds(findings, "up"))

    def test_distant_second_press_is_not_double(self):
        """Two deliberate taps a second apart are two taps, not a bounce."""
        s = stream([(5, hold("up"), [0x80] * 4, False),
                    (400, NONE, [0x80] * 4, False),
                    (5, hold("up"), [0x80] * 4, False)])
        _, findings = pt.analyze(s)
        self.assertNotIn("double_press", self.kinds(findings, "up"))

    def test_dpad_fold_detected(self):
        """D-pad bit + deflected left stick presented together."""
        s = stream([(10, NONE, [0x80] * 4, True),
                    (10, hold("up"), [0x80, 0x00, 0x80, 0x80], True),
                    (10, NONE, [0x80] * 4, True)])
        _, findings = pt.analyze(s)
        self.assertIn("dpad_fold", self.kinds(findings, "up"))

    def test_fold_not_reported_for_face_buttons(self):
        """A stick deflection during a Cross press is not the fold bug."""
        s = stream([(10, NONE, [0x80] * 4, True),
                    (10, hold("cross"), [0x80, 0x00, 0x80, 0x80], True),
                    (10, NONE, [0x80] * 4, True)])
        _, findings = pt.analyze(s)
        self.assertNotIn("dpad_fold", self.kinds(findings, "cross"))

    def test_analog_flip_detected(self):
        s = stream([(5, hold("up"), [0x80] * 4, True),
                    (5, hold("up"), [0x80] * 4, False)])
        _, findings = pt.analyze(s)
        self.assertIn("analog_flip", self.kinds(findings, "up"))

    def test_long_hold_detected(self):
        """A press spanning enough frames for menu auto-repeat."""
        s = stream([(5, NONE, [0x80] * 4, False),
                    (200, hold("up"), [0x80] * 4, False),
                    (5, NONE, [0x80] * 4, False)])
        _, findings = pt.analyze(s)
        self.assertIn("long_hold", self.kinds(findings, "up"))

    def test_short_tap_is_not_long_hold(self):
        s = stream([(5, NONE, [0x80] * 4, False),
                    (10, hold("up"), [0x80] * 4, False),
                    (5, NONE, [0x80] * 4, False)])
        _, findings = pt.analyze(s)
        self.assertNotIn("long_hold", self.kinds(findings, "up"))

    def test_runs_sorted_by_time(self):
        s = stream([(5, hold("down"), [0x80] * 4, False),
                    (5, NONE, [0x80] * 4, False),
                    (5, hold("up"), [0x80] * 4, False)])
        runs, _ = pt.analyze(s)
        self.assertEqual([r["button"] for r in runs], ["down", "up"])

    def test_frames_counted_from_emulated_frame_numbers(self):
        s = [sample(0.00, 100, hold("up")),
             sample(0.01, 103, hold("up")),
             sample(0.02, 105, NONE)]
        runs, _ = pt.analyze(s)
        self.assertEqual(runs[0]["frames"], 4)  # 100..103 inclusive


if __name__ == "__main__":
    unittest.main()


class FoldSkewTest(unittest.TestCase):
    """The D-pad bit and its folded stick deflection should arrive together."""

    def test_simultaneous_fold_has_zero_skew(self):
        s = [sample(0.00, 10, NONE),
             sample(0.01, 11, hold("up"), [0x80, 0x00, 0x80, 0x80], True),
             sample(0.02, 12, NONE)]
        runs, findings = pt.analyze(s)
        self.assertEqual(runs[0]["fold_skew_frames"], 0)
        self.assertNotIn("fold_skew", {f["kind"] for f in findings})

    def test_late_stick_is_reported_as_skew(self):
        s = [sample(0.00, 10, hold("up"), [0x80] * 4, True),
             sample(0.01, 12, hold("up"), [0x80, 0x00, 0x80, 0x80], True),
             sample(0.02, 13, NONE)]
        runs, findings = pt.analyze(s)
        self.assertEqual(runs[0]["fold_skew_frames"], 2)
        self.assertIn("fold_skew", {f["kind"] for f in findings})

    def test_no_stick_means_no_skew(self):
        s = [sample(0.00, 10, hold("up"), [0x80] * 4, False),
             sample(0.01, 11, NONE)]
        runs, findings = pt.analyze(s)
        self.assertIsNone(runs[0]["fold_skew_frames"])
        self.assertNotIn("fold_skew", {f["kind"] for f in findings})
