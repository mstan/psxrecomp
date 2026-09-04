#!/usr/bin/env python3
"""Unit tests for cursor_writers' attribution of cursor writes."""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import cursor_writers as cw


def w(addr, old, new, pc, seq=0, width=1, frame=0, func="0x1000"):
    return {"seq": seq, "addr": "0x%08X" % (addr & 0x1FFFFFFF),
            "old": "0x%08X" % old, "new": "0x%08X" % new,
            "pc": pc, "func": func, "ra": "0x2000", "w": width, "frame": frame}


class PhysTest(unittest.TestCase):
    def test_kseg0_maps_to_physical(self):
        self.assertEqual(cw.phys(0x8004A5F5), 0x0004A5F5)

    def test_physical_unchanged(self):
        self.assertEqual(cw.phys(0x0004A5F5), 0x0004A5F5)


class DeltaTest(unittest.TestCase):
    def test_simple_decrement(self):
        self.assertEqual(cw.delta(w(0x8004A5F5, 5, 4, "0x1")), -1)

    def test_double_step(self):
        self.assertEqual(cw.delta(w(0x8004A5F5, 5, 3, "0x1")), -2)

    def test_byte_wrap_reads_as_small_negative(self):
        """0 -> 255 at byte width is -1, not +255."""
        self.assertEqual(cw.delta(w(0x8004A5F5, 0, 255, "0x1", width=1)), -1)

    def test_word_width_increment(self):
        self.assertEqual(cw.delta(w(0x8004A5F5, 7, 8, "0x1", width=4)), 1)


class RangeTest(unittest.TestCase):
    def test_filters_and_sorts(self):
        entries = [w(0x8004A5F5, 5, 4, "0x1", seq=9),
                   w(0x8004B000, 1, 2, "0x2", seq=8),
                   w(0x8004A50D, 7, 6, "0x3", seq=7)]
        got = cw.writes_in_range(entries, 0x8004A500, 0x8004A600)
        self.assertEqual([e["seq"] for e in got], [7, 9])

    def test_upper_bound_is_exclusive(self):
        entries = [w(0x8004A600, 1, 2, "0x1")]
        self.assertEqual(cw.writes_in_range(entries, 0x8004A500, 0x8004A600), [])


class VerdictTest(unittest.TestCase):
    def test_no_writes(self):
        self.assertEqual(cw.verdict([])[0], "no_writes")

    def test_single_write_double_step(self):
        """One write of -2: the step calculation doubled, not the input."""
        got = cw.verdict([w(0x8004A5F5, 5, 3, "0x800684C0")], 0x8004A5F5)
        self.assertEqual(got[0], "single_write_double_step")

    def test_single_write_single_step(self):
        got = cw.verdict([w(0x8004A5F5, 5, 4, "0x800684C0")], 0x8004A5F5)
        # Capture-level verdict: nothing doubled anywhere.
        self.assertEqual(got[0], "no_double_step")
        # The event itself is still classified as a clean single step.
        ev = cw.describe_event([w(0x8004A5F5, 5, 4, "0x800684C0")], 0x8004A5F5)
        self.assertEqual(ev["kind"], "single_write_single_step")
        self.assertEqual(ev["total"], -1)

    def test_two_distinct_pcs_is_two_paths(self):
        got = cw.verdict([w(0x8004A5F5, 5, 4, "0x11110000", seq=1),
                          w(0x8004A5F5, 4, 3, "0x22220000", seq=2)],
                         0x8004A5F5)
        self.assertEqual(got[0], "two_paths")

    def test_same_pc_twice_is_one_path(self):
        got = cw.verdict([w(0x8004A5F5, 5, 4, "0x11110000", seq=1),
                          w(0x8004A5F5, 4, 3, "0x11110000", seq=2)],
                         0x8004A5F5)
        self.assertEqual(got[0], "one_path_twice")


class SummarizeTest(unittest.TestCase):
    def test_groups_by_address_and_collects_pcs(self):
        writes = [w(0x8004A5F5, 5, 4, "0xA", seq=1),
                  w(0x8004A5F5, 4, 3, "0xB", seq=2),
                  w(0x8004A50D, 7, 6, "0xC", seq=3)]
        by_addr, focus = cw.summarize(writes, 0x8004A5F5)
        self.assertEqual(len(by_addr), 2)
        self.assertEqual(by_addr[0x0004A5F5]["pcs"], ["0xA", "0xB"])
        self.assertEqual(len(focus), 2)


class WidthCoverageTest(unittest.TestCase):
    """A wide store changes bytes that never appear as its address."""

    def test_halfword_write_covers_high_byte(self):
        e = w(0x8004A5F4, 0x05F9, 0x0366, "0x80024E3C", width=2)
        self.assertTrue(cw.covers(e, 0x8004A5F5))

    def test_write_does_not_cover_beyond_its_width(self):
        e = w(0x8004A5F4, 0, 1, "0x1", width=2)
        self.assertFalse(cw.covers(e, 0x8004A5F6))

    def test_byte_value_extracts_the_right_lane(self):
        e = w(0x8004A5F4, 0x05F9, 0x0366, "0x1", width=2)
        self.assertEqual(cw.byte_value(e, "old", 0x8004A5F5), 5)
        self.assertEqual(cw.byte_value(e, "new", 0x8004A5F5), 3)
        self.assertEqual(cw.byte_value(e, "old", 0x8004A5F4), 0xF9)

    def test_byte_delta_of_covered_byte(self):
        e = w(0x8004A5F4, 0x05F9, 0x0366, "0x1", width=2)
        self.assertEqual(cw.byte_delta(e, 0x8004A5F5), -2)

    def test_wide_write_leaving_the_byte_alone_is_not_a_move(self):
        """Covering the byte is not the same as changing it."""
        e = w(0x8004A5F4, 0x0500, 0x05FF, "0x1", width=2)
        self.assertEqual(cw.byte_delta(e, 0x8004A5F5), 0)
        self.assertEqual(cw.touching_writes([e], 0x8004A5F5), [])

    def test_touching_writes_finds_wide_stores(self):
        es = [w(0x8004A5F4, 0x05F9, 0x0366, "0x1", width=2),
              w(0x8004A5F4, 0x0366, 0x0823, "0x1", width=2)]
        self.assertEqual(len(cw.touching_writes(es, 0x8004A5F5)), 2)

    def test_verdict_uses_byte_delta_not_word_delta(self):
        """-2 on the byte, though the halfword moved by -659."""
        e = w(0x8004A5F4, 0x05F9, 0x0366, "0x1", width=2)
        kind, _ = cw.verdict([e], 0x8004A5F5)
        self.assertEqual(kind, "single_write_double_step")


class GroupEventsTest(unittest.TestCase):
    """Several presses in one capture must not be read as one event."""

    def test_writes_in_the_same_frame_are_one_event(self):
        ws = [w(0x8004A5F5, 5, 4, "0xA", seq=1, frame=100),
              w(0x8004A5F5, 4, 3, "0xB", seq=2, frame=100)]
        self.assertEqual(len(cw.group_events(ws, 0x8004A5F5)), 1)

    def test_writes_seconds_apart_are_separate_events(self):
        ws = [w(0x8004A5F5, 5, 4, "0xA", seq=1, frame=100),
              w(0x8004A5F5, 4, 3, "0xA", seq=2, frame=250)]
        self.assertEqual(len(cw.group_events(ws, 0x8004A5F5)), 2)

    def test_adjacent_frames_stay_together(self):
        ws = [w(0x8004A5F5, 5, 4, "0xA", seq=1, frame=100),
              w(0x8004A5F5, 4, 3, "0xB", seq=2, frame=102)]
        self.assertEqual(len(cw.group_events(ws, 0x8004A5F5)), 1)


class PerPressVerdictTest(unittest.TestCase):
    def test_two_paths_on_a_doubled_press(self):
        ws = [w(0x8004A5F5, 5, 4, "0x11110000", seq=1, frame=100),
              w(0x8004A5F5, 4, 3, "0x22220000", seq=2, frame=100)]
        self.assertEqual(cw.verdict(ws, 0x8004A5F5)[0], "two_paths")

    def test_one_path_twice_on_a_doubled_press(self):
        ws = [w(0x8004A5F5, 5, 4, "0x11110000", seq=1, frame=100),
              w(0x8004A5F5, 4, 3, "0x11110000", seq=2, frame=101)]
        self.assertEqual(cw.verdict(ws, 0x8004A5F5)[0], "one_path_twice")

    def test_single_write_of_two_steps(self):
        ws = [w(0x8004A5F5, 5, 3, "0x11110000", seq=1, frame=100)]
        self.assertEqual(cw.verdict(ws, 0x8004A5F5)[0],
                         "single_write_double_step")

    def test_clean_presses_report_no_doubling(self):
        ws = [w(0x8004A5F5, 5, 4, "0xA", seq=1, frame=100),
              w(0x8004A5F5, 4, 3, "0xA", seq=2, frame=300)]
        self.assertEqual(cw.verdict(ws, 0x8004A5F5)[0], "no_double_step")

    def test_mixed_capture_reports_the_doubled_presses(self):
        """Single-step presses in the same capture must not mask the bug."""
        ws = [w(0x8004A5F5, 9, 8, "0xA", seq=1, frame=100),
              w(0x8004A5F5, 8, 7, "0xA", seq=2, frame=300),
              w(0x8004A5F5, 7, 6, "0xB", seq=3, frame=500),
              w(0x8004A5F5, 6, 5, "0xC", seq=4, frame=500)]
        self.assertEqual(cw.verdict(ws, 0x8004A5F5)[0], "two_paths")
