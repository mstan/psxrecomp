#!/usr/bin/env python3
"""Unit tests for menu_scan's cursor identification."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import menu_scan as ms
sys.path.insert(0, os.path.dirname(__file__))
from test_pad_delivery import txn, hold, NONE


class FindChangedOffsetsTest(unittest.TestCase):
    def test_no_change(self):
        a = bytes(4096)
        self.assertEqual(ms.find_changed_offsets(a, a), [])

    def test_single_byte_change(self):
        a = bytearray(4096)
        b = bytearray(4096)
        b[1234] = 7
        self.assertEqual(ms.find_changed_offsets(bytes(a), bytes(b)), [1234])

    def test_changes_across_block_boundary(self):
        a = bytearray(4096)
        b = bytearray(4096)
        b[1023] = 1
        b[1024] = 1
        self.assertEqual(ms.find_changed_offsets(bytes(a), bytes(b)),
                         [1023, 1024])

    def test_ragged_tail_shorter_than_block(self):
        a = bytes(1500)
        b = bytearray(1500)
        b[1400] = 9
        self.assertEqual(ms.find_changed_offsets(a, bytes(b)), [1400])


class HeldSpansTest(unittest.TestCase):
    """Holding a direction is input for its whole duration, not one edge."""

    def build(self, words):
        entries, seq = [], 0
        for w in words:
            entries.extend(txn(seq, w))
            seq += 16
        return entries

    def test_single_tap_is_one_span(self):
        e = self.build([NONE, hold("down"), hold("down"), NONE])
        self.assertEqual(len(ms.held_spans(e, "down")), 1)

    def test_long_hold_is_one_span_covering_it(self):
        e = self.build([NONE] + [hold("down")] * 6 + [NONE])
        spans = ms.held_spans(e, "down")
        self.assertEqual(len(spans), 1)
        self.assertGreater(spans[0][1] - spans[0][0], 60)

    def test_two_taps_are_two_spans(self):
        e = self.build([hold("down"), NONE, hold("down"), NONE])
        self.assertEqual(len(ms.held_spans(e, "down")), 2)

    def test_still_held_at_capture_end_is_closed(self):
        e = self.build([NONE, hold("down"), hold("down")])
        self.assertEqual(len(ms.held_spans(e, "down")), 1)

    def test_other_button_ignored(self):
        e = self.build([hold("up"), NONE])
        self.assertEqual(ms.held_spans(e, "down"), [])


class InputIntervalsTest(unittest.TestCase):
    def test_press_marks_its_interval(self):
        before, after = [0, 100, 200], [10, 110, 210]
        allowed, presses = ms.input_intervals(before, after,
                                              {"down": [(50, 55)]}, 0)
        self.assertEqual(allowed, {0})
        self.assertEqual(presses, [("down", 0, 0)])

    def test_press_during_a_read_marks_both_neighbours(self):
        before, after = [0, 100, 200], [10, 110, 210]
        allowed, _ = ms.input_intervals(before, after,
                                        {"down": [(105, 106)]}, 0)
        self.assertEqual(allowed, {0, 1})

    def test_every_direction_counts_as_input(self):
        """A cursor moves on up as well as down; both must be allowed."""
        before, after = [0, 100, 200, 300], [10, 110, 210, 310]
        allowed, presses = ms.input_intervals(
            before, after, {"up": [(50, 55)], "down": [(250, 255)]}, 0)
        self.assertEqual(allowed, {0, 2})
        self.assertEqual([p[0] for p in presses], ["up", "down"])

    def test_presses_returned_in_capture_order(self):
        before, after = [0, 100, 200, 300], [10, 110, 210, 310]
        _, presses = ms.input_intervals(
            before, after, {"up": [(250, 255)], "down": [(50, 55)]}, 0)
        self.assertEqual([p[0] for p in presses], ["down", "up"])

    def test_long_hold_marks_every_interval_it_spans(self):
        before, after = [0, 100, 200, 300], [10, 110, 210, 310]
        allowed, _ = ms.input_intervals(before, after,
                                        {"down": [(50, 250)]}, 0)
        self.assertEqual(allowed, {0, 1, 2})

    def test_span_outside_capture_not_located(self):
        before, after = [100, 200], [110, 210]
        allowed, presses = ms.input_intervals(before, after,
                                              {"down": [(500, 510)]}, 0)
        self.assertEqual(allowed, set())
        self.assertEqual(presses, [])


class PressDeltasTest(unittest.TestCase):
    """Measure the step settled-to-settled, so animation is not sampled."""

    def test_single_step_per_press(self):
        values = [10, 10, 9, 9, 8, 8]
        presses = [("down", 1, 1), ("down", 3, 3)]
        self.assertEqual(ms.press_deltas(values, presses, len(values)),
                         [("down", -1), ("down", -1)])

    def test_doubled_press_shows_double_step(self):
        """The bug: one press moved two entries."""
        values = [10, 10, 9, 9, 7, 7]
        presses = [("down", 1, 1), ("down", 3, 3)]
        self.assertEqual(ms.press_deltas(values, presses, len(values)),
                         [("down", -1), ("down", -2)])

    def test_opposite_directions_reported_separately(self):
        values = [10, 10, 11, 11, 10, 10]
        presses = [("down", 1, 1), ("up", 3, 3)]
        self.assertEqual(ms.press_deltas(values, presses, len(values)),
                         [("down", 1), ("up", -1)])

    def test_animation_spanning_settle_is_measured_whole(self):
        """Value slides 10 -> 8 across the settle window: one step of -2."""
        values = [10, 10, 9, 8, 8]
        presses = [("down", 1, 2)]
        self.assertEqual(ms.press_deltas(values, presses, len(values)),
                         [("down", -2)])

    def test_press_at_the_very_end_does_not_overrun(self):
        values = [10, 9]
        presses = [("down", 0, 0)]
        self.assertEqual(ms.press_deltas(values, presses, len(values)),
                         [("down", -1)])


class ByteDeltaTest(unittest.TestCase):
    def test_plain_step(self):
        self.assertEqual(ms.byte_delta(3, 4), 1)

    def test_wrap_down(self):
        """0 -> 255 is a step back, not a jump of 255."""
        self.assertEqual(ms.byte_delta(0, 255), -1)

    def test_wrap_up(self):
        self.assertEqual(ms.byte_delta(255, 0), 1)


class ClassifyOffsetTest(unittest.TestCase):
    """values indices line up with snapshots; presses give (button, lo, hi)."""

    def test_axis_moves_opposite_ways_and_is_strict(self):
        """up and down move a cursor opposite ways -- not a disqualifier."""
        values = [10, 10, 9, 9, 8, 8, 9, 9]
        presses = [("down", 1, 1), ("down", 3, 3), ("up", 5, 5)]
        allowed = {1, 2, 3, 4, 5, 6}
        sc = ms.classify_offset(values, allowed, presses)
        self.assertIsNotNone(sc)
        self.assertTrue(sc["strict"])
        self.assertTrue(sc["opposed"])

    def test_movement_without_input_disqualifies(self):
        values = [10, 11, 12, 13]
        presses = [("down", 0, 0)]
        self.assertIsNone(ms.classify_offset(values, {0}, presses))

    def test_double_step_flagged(self):
        """One press moved two strides: the bug, at whatever stride."""
        values = [100, 100, 72, 72, 44, 44, 244, 244]
        presses = [("right", 1, 1), ("right", 3, 3), ("right", 5, 5)]
        allowed = {1, 2, 3, 4, 5, 6}
        sc = ms.classify_offset(values, allowed, presses)
        self.assertTrue(sc["strict"])
        self.assertTrue(sc["doubled"])
        self.assertEqual(sc["base"], 28)

    def test_same_button_moving_both_ways_is_not_strict(self):
        """`down` must not move a cursor up on one press and down the next."""
        values = [10, 10, 9, 9, 10, 10, 9, 9]
        presses = [("down", 1, 1), ("down", 3, 3), ("down", 5, 5)]
        allowed = {1, 2, 3, 4, 5, 6}
        sc = ms.classify_offset(values, allowed, presses)
        self.assertFalse(sc["strict"])
        self.assertFalse(sc["consistent"])

    def test_too_few_responses_is_not_strict(self):
        values = [10, 10, 9, 9, 9, 9]
        presses = [("down", 1, 1), ("down", 3, 3)]
        sc = ms.classify_offset(values, {1, 2, 3, 4}, presses, min_moves=3)
        self.assertFalse(sc["strict"])

    def test_irregular_stride_is_not_strict(self):
        values = [0, 0, 3, 3, 13, 13, 20, 20]
        presses = [("down", 1, 1), ("down", 3, 3), ("down", 5, 5)]
        sc = ms.classify_offset(values, {1, 2, 3, 4, 5, 6}, presses)
        self.assertFalse(sc["strict"])

    def test_never_moving_is_not_a_candidate(self):
        values = [7] * 6
        presses = [("down", 1, 1)]
        self.assertIsNone(ms.classify_offset(values, {1, 2}, presses))


class DecodeTapsTest(unittest.TestCase):
    def test_rising_edges_only(self):
        entries = []
        seq = 0
        for w in [NONE, hold("down"), hold("down"), NONE, hold("down"), NONE]:
            entries.extend(txn(seq, w))
            seq += 16
        taps = ms.decode_taps(entries, "down")
        self.assertEqual(len(taps), 2)

    def test_held_across_whole_capture_is_one_tap(self):
        entries = []
        seq = 0
        for w in [hold("down")] * 5:
            entries.extend(txn(seq, w))
            seq += 16
        self.assertEqual(len(ms.decode_taps(entries, "down")), 1)

    def test_other_button_ignored(self):
        entries = []
        seq = 0
        for w in [NONE, hold("up"), NONE]:
            entries.extend(txn(seq, w))
            seq += 16
        self.assertEqual(ms.decode_taps(entries, "down"), [])


if __name__ == "__main__":
    unittest.main()


class DirectionTest(unittest.TestCase):
    """Per-button consistency replaces the old global sign test."""

    def test_one_direction_moving_both_ways_rejected(self):
        values = [10, 10, 13, 13, 10, 10]
        presses = [("down", 1, 1), ("down", 3, 3)]
        sc = ms.classify_offset(values, {1, 2, 3, 4}, presses, min_moves=2)
        self.assertFalse(sc["strict"])

    def test_opposite_buttons_moving_opposite_ways_accepted(self):
        values = [10, 10, 11, 11, 10, 10, 11, 11]
        presses = [("up", 1, 1), ("down", 3, 3), ("up", 5, 5)]
        sc = ms.classify_offset(values, {1, 2, 3, 4, 5, 6}, presses)
        self.assertTrue(sc["strict"])
        self.assertTrue(sc["opposed"])
