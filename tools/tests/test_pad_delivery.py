#!/usr/bin/env python3
"""Unit tests for pad_delivery's SIO reconstruction."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pad_delivery as pd


NONE = 0xFFFF


def txn(seq, buttons=NONE, pad_id=0x73, sticks=(0x80, 0x80, 0x80, 0x80),
        addr=0x01, cmd=0x42, truncate=None, frame=0):
    """Build the SIO byte entries for one controller transaction."""
    lo, hi = buttons & 0xFF, (buttons >> 8) & 0xFF
    lx, ly, rx, ry = sticks
    pairs = [(addr, 0xFF), (cmd, pad_id), (0x00, 0x5A), (0x00, lo), (0x00, hi),
             (0x00, rx), (0x00, ry), (0x00, lx), (0x00, ly)]
    if truncate is not None:
        pairs = pairs[:truncate]
    return [{"seq": seq + i, "tx": "0x%02X" % t, "rx": "0x%02X" % r,
             "frame": frame}
            for i, (t, r) in enumerate(pairs)]


def hold(name):
    return NONE & ~pd.BUTTONS[name]


def stream(words, start=0, frame_of=None):
    out, seq = [], start
    for i, w in enumerate(words):
        f = frame_of(i) if frame_of else i
        out.extend(txn(seq, w, frame=f))
        seq += 16
    return out


class DecodeTest(unittest.TestCase):
    def test_single_poll_decoded(self):
        polls = pd.decode_polls(txn(100, hold("down")))
        self.assertEqual(len(polls), 1)
        self.assertEqual(polls[0]["buttons"], hold("down"))
        self.assertEqual(polls[0]["id"], 0x73)

    def test_sticks_decoded_in_lx_ly_order(self):
        polls = pd.decode_polls(txn(0, NONE, sticks=(0x11, 0x22, 0x33, 0x44)))
        self.assertEqual(polls[0]["sticks"], [0x11, 0x22, 0x33, 0x44])

    def test_memory_card_traffic_ignored(self):
        entries = txn(0, NONE, addr=0x81) + txn(50, hold("up"))
        polls = pd.decode_polls(entries)
        self.assertEqual(len(polls), 1)
        self.assertEqual(polls[0]["buttons"], hold("up"))

    def test_truncated_transaction_dropped(self):
        """A transaction cut off before the button word carries no state."""
        polls = pd.decode_polls(txn(0, hold("up"), truncate=3))
        self.assertEqual(polls, [])

    def test_non_poll_command_dropped(self):
        """0x43 (config) is not a button poll."""
        polls = pd.decode_polls(txn(0, hold("up"), cmd=0x43))
        self.assertEqual(polls, [])

    def test_multiple_polls_in_order(self):
        polls = pd.decode_polls(stream([NONE, hold("up"), NONE]))
        self.assertEqual([p["buttons"] for p in polls],
                         [NONE, hold("up"), NONE])

    def test_digital_pad_id(self):
        polls = pd.decode_polls(txn(0, NONE, pad_id=0x41, truncate=5))
        self.assertEqual(polls[0]["id"], 0x41)
        self.assertIsNone(polls[0]["sticks"])


class RunTest(unittest.TestCase):
    def test_contiguous_press_is_one_run(self):
        polls = pd.decode_polls(stream([NONE] + [hold("down")] * 4 + [NONE]))
        self.assertEqual(pd.poll_runs(polls, "down"), [(1, 4)])

    def test_split_press_is_two_runs(self):
        polls = pd.decode_polls(
            stream([NONE, hold("down"), hold("down"), NONE,
                    hold("down"), hold("down"), NONE]))
        self.assertEqual(pd.poll_runs(polls, "down"), [(1, 2), (4, 5)])


class AnalyzeTest(unittest.TestCase):
    def kinds(self, findings):
        return {f["kind"] for f in findings}

    def test_clean_press_reports_nothing(self):
        polls = pd.decode_polls(stream([NONE] + [hold("down")] * 4 + [NONE]))
        runs, findings = pd.analyze(polls)
        self.assertEqual(len(runs), 1)
        self.assertEqual(findings, [])

    def test_mid_press_release_is_a_bounce(self):
        """The smoking gun: one tap delivered as two rising edges."""
        polls = pd.decode_polls(
            stream([NONE, hold("down"), hold("down"), NONE,
                    hold("down"), hold("down"), NONE]))
        _, findings = pd.analyze(polls)
        self.assertIn("delivered_bounce", self.kinds(findings))

    def test_two_deliberate_taps_are_not_a_bounce(self):
        words = ([NONE] + [hold("down")] * 3 + [NONE] * 40
                 + [hold("down")] * 3 + [NONE])
        polls = pd.decode_polls(stream(words))
        _, findings = pd.analyze(polls)
        self.assertNotIn("delivered_bounce", self.kinds(findings))

    def test_polls_per_frame(self):
        """Two polls landing in the same frame are counted together."""
        polls = pd.decode_polls(stream([NONE] * 4, frame_of=lambda i: i // 2))
        self.assertEqual(pd.polls_per_frame(polls), {0: 2, 1: 2})


if __name__ == "__main__":
    unittest.main()


class ResolveFrameTest(unittest.TestCase):
    """A batch may straddle a frame boundary; say -1 rather than guess."""

    def test_stable_counter_gives_exact_frame(self):
        self.assertEqual(pd.resolve_frame(1100, 1100), 1100)

    def test_advanced_counter_is_unknowable(self):
        self.assertEqual(pd.resolve_frame(1100, 1101), -1)

    def test_first_batch_has_no_predecessor(self):
        self.assertEqual(pd.resolve_frame(None, 1100), -1)

    def test_missing_counter_is_unknowable(self):
        self.assertEqual(pd.resolve_frame(1100, -1), -1)

    def test_unstamped_polls_excluded_from_counts(self):
        polls = [{"seq": 0, "frame": -1, "buttons": NONE},
                 {"seq": 10, "frame": 5, "buttons": NONE},
                 {"seq": 20, "frame": 5, "buttons": NONE}]
        self.assertEqual(pd.polls_per_frame(polls), {5: 2})
