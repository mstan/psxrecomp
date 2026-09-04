#!/usr/bin/env python3
"""Focused tests for stall_report.py evidence semantics."""

import importlib.util
import json
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "stall_report", ROOT / "stall_report.py")
STALL = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(STALL)


def reply_for(name, params):
    if name == "overlay_loader_status":
        return {"ok": True, "dispatch_native": 100,
                "dispatch_interp_fallback": 20, "stale_blocked": 0}
    if name == "autocompile_status":
        return {"ok": True, "compile": {
            "configured": 1, "degraded": 0, "degraded_reason": "",
            "runs": 1, "fails": 0, "consecutive_fails": 0,
            "state": "idle", "last_exit": 0, "shard_result_seen": 1,
            "shard_ok": 2, "shard_fail": 0, "shard_skipped": 0,
            "shard_fail_total": 0}}
    if name == "dirty_ram_stats":
        return {"ok": True, "blocks_run": 2, "insns_run": 20,
                "native_handoffs": 1, "text_native_blocked": 0,
                "text_diverged_pages": 0, "per_pc": []}
    if name == "kernel_bless":
        return {"ok": True, "entries": 0, "clean": 0, "mismatch": 0,
                "native_hits": 0}
    if name == "frame_perf":
        return {"ok": True}
    if name == "phase_profile":
        return {"ok": True, "window_s": params["window"], "samples": 100,
                "interp_share": 0.1, "native_share": 0.2,
                "static_share": 0.6, "gpu_share": 0.05,
                "other_share": 0.05, "exc_share": 0.1}
    if name == "phase_hot":
        return {"ok": True, "set": params["set"],
                "phase_samples_total": 0, "hash_drops": 0, "top": []}
    if name == "starv_ring":
        return {"ok": True, "total": 2, "returned": 2, "entries": [
            {"seq": 0, "us": 1000, "cyc": 100, "func": "0x1", "in_exc": 0},
            {"seq": 1, "us": 2000, "cyc": 200, "func": "0x2", "in_exc": 0},
        ]}
    if name == "ping":
        return {"ok": True}
    raise AssertionError(name)


class FakeClient:
    def __init__(self, override=None):
        self.override = override or {}
        self.calls = []

    def cmd(self, name, **params):
        self.calls.append((name, params))
        value = self.override.get(name)
        return value if value is not None else reply_for(name, params)


class FakeSocket:
    def __init__(self, chunks):
        self.chunks = list(chunks)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def sendall(self, body):
        self.request = json.loads(body)

    def recv(self, size):
        return self.chunks.pop(0) if self.chunks else b""


class StallReportTests(unittest.TestCase):
    def test_client_rejects_response_id_mismatch(self):
        socket = FakeSocket([b'{"id":99,"ok":true}\n'])
        with mock.patch.object(STALL.socket, "create_connection",
                               return_value=socket):
            reply = STALL.Client("127.0.0.1", 1).cmd("ping")
        self.assertFalse(reply["ok"])
        self.assertIn("response id mismatch", reply["error"])

    def test_client_accepts_lock_free_ping_fast_path_id(self):
        socket = FakeSocket([
            b'{"id":0,"ok":true,"pong":true,"io_thread":true}\n'])
        with mock.patch.object(STALL.socket, "create_connection",
                               return_value=socket):
            reply = STALL.Client("127.0.0.1", 1).cmd("ping")
        self.assertTrue(reply["ok"])
        self.assertEqual(reply["id"], 0)

    def test_client_reassembles_fragmented_response(self):
        socket = FakeSocket([b'{"id":1,', b'"ok":true}\n'])
        with mock.patch.object(STALL.socket, "create_connection",
                               return_value=socket):
            reply = STALL.Client("127.0.0.1", 1).cmd("ping")
        self.assertTrue(reply["ok"])

    def test_snapshot_requests_bounded_window_and_full_hot_set(self):
        client = FakeClient()
        snap = STALL.snapshot(client, phase_window=42)
        self.assertEqual(snap["phase_window_requested_s"], 42)
        self.assertIn(("phase_profile", {"window": 42}), client.calls)
        self.assertIn(("phase_hot", {"set": "native", "top": 64}), client.calls)

    def test_snapshot_fails_closed_on_missing_command(self):
        client = FakeClient({"dirty_ram_stats": {"ok": False,
                                                  "error": "not supported"}})
        with self.assertRaisesRegex(STALL.ReportError, "dirty_ram_stats failed"):
            STALL.snapshot(client)

    def test_snapshot_retains_optional_frame_perf_failure(self):
        client = FakeClient({"frame_perf": {"ok": False,
                                             "error": "no samples"}})
        snap = STALL.snapshot(client)
        self.assertFalse(snap["frame_perf"]["ok"])
        self.assertIn("optional frame_perf unavailable", snap["warnings"][0])

    def test_counter_reset_fails_closed(self):
        first = {"overlay_loader_status": {"dispatch_native": 100}}
        second = {"overlay_loader_status": {"dispatch_native": 2}}
        with self.assertRaisesRegex(STALL.ReportError, "counter reset"):
            STALL.delta(first, second, "overlay_loader_status", "dispatch_native")

    def test_dispatch_counts_are_not_reported_as_percentages(self):
        snap = STALL.snapshot(FakeClient())
        lines = "\n".join(STALL.verdict_lines(None, snap))
        self.assertIn("dispatch_native", lines)
        self.assertIn("does not convert them into a coverage percentage", lines)
        self.assertNotIn("83.33%", lines)

    def test_nested_autocompile_failure_is_visible(self):
        client = FakeClient()
        snap = STALL.snapshot(client)
        snap["autocompile_status"]["compile"].update(
            {"fails": 1, "shard_fail": 3, "shard_fail_total": 3})
        lines = "\n".join(STALL.verdict_lines(None, snap))
        self.assertIn("fails=1", lines)
        self.assertIn("shard_fail=3", lines)
        self.assertIn("Shards are FAILING", lines)

    def test_unconfigured_autocompile_zeroes_are_explained(self):
        snap = STALL.snapshot(FakeClient())
        snap["autocompile_status"]["compile"]["configured"] = 0
        lines = "\n".join(STALL.verdict_lines(None, snap))
        self.assertIn("autocompile is NOT CONFIGURED", lines)

    def test_configured_without_result_is_not_reported_healthy(self):
        snap = STALL.snapshot(FakeClient())
        snap["autocompile_status"]["compile"]["shard_result_seen"] = 0
        lines = "\n".join(STALL.verdict_lines(None, snap))
        self.assertIn("no PSX_SHARD_RESULT has been observed", lines)

    def test_missing_nested_autocompile_object_fails_closed(self):
        snap = STALL.snapshot(FakeClient())
        snap["autocompile_status"].pop("compile")
        with self.assertRaisesRegex(STALL.ReportError, "missing object"):
            STALL.verdict_lines(None, snap)

    def test_wrapped_ring_scope_is_explicit(self):
        entries = [
            {"seq": 20000, "us": 1000, "cyc": 100, "func": "0x1", "in_exc": 0},
            {"seq": 20001, "us": 2000, "cyc": 200, "func": "0x2", "in_exc": 0},
        ]
        client = FakeClient({"starv_ring": {"ok": True, "total": 20002,
                                             "returned": 2, "entries": entries}})
        span, error, gaps, meta = STALL.stalls_from_ring(client)
        self.assertIsNone(error)
        self.assertEqual(span, 1.0)
        self.assertEqual(len(gaps), 1)
        self.assertTrue(meta["ring_has_wrapped"])
        self.assertEqual(meta["scope"], "last returned PC samples only")
        self.assertEqual(meta["raw_entries"], entries)

    def test_non_monotonic_ring_abstains_from_gap_math(self):
        entries = [
            {"seq": 2, "us": 2000, "cyc": 200, "func": "0x1", "in_exc": 0},
            {"seq": 1, "us": 1000, "cyc": 100, "func": "0x2", "in_exc": 0},
        ]
        client = FakeClient({"starv_ring": {"ok": True, "total": 2,
                                             "returned": 2, "entries": entries}})
        span, error, gaps, meta = STALL.stalls_from_ring(client)
        self.assertIsNone(span)
        self.assertIn("non-monotonic", error)
        self.assertEqual(gaps, [])
        self.assertEqual(meta["raw_entries"], entries)

    def test_hot_set_reports_censoring(self):
        reply = {"hash_drops": 0,
                 "top": [{"addr": hex(index), "samples": 1}
                         for index in range(64)]}
        warning = STALL.hot_set_warning(reply, 64)
        self.assertIn("censored", warning)

    def test_hot_set_reports_hash_drops(self):
        warning = STALL.hot_set_warning({"hash_drops": 3, "top": []}, 64)
        self.assertIn("dropped 3 samples", warning)

    def test_ring_missing_sequence_fails_closed(self):
        entries = [
            {"us": 1000, "cyc": 100, "func": "0x1", "in_exc": 0},
            {"seq": 1, "us": 2000, "cyc": 200, "func": "0x2", "in_exc": 0},
        ]
        client = FakeClient({"starv_ring": {"ok": True, "total": 2,
                                             "returned": 2, "entries": entries}})
        with self.assertRaisesRegex(STALL.ReportError, r"entries\[0\].seq"):
            STALL.stalls_from_ring(client)

    def test_required_counter_rejects_float(self):
        with self.assertRaisesRegex(STALL.ReportError, "invalid numeric field"):
            STALL.require_num({"count": 1.5}, "count", "fixture")

    def test_phase_hot_empty_does_not_hide_unlocalized_samples(self):
        first = STALL.snapshot(FakeClient())
        second = STALL.snapshot(FakeClient())
        second["wall"] = first["wall"] + 1
        second["phase_hot:native"]["phase_samples_total"] = 12
        text = STALL.report(first, second, 1.0, None, [], {})
        self.assertIn("localization unavailable for 12 native samples", text)

    def test_dirty_pc_accounting_reports_partial_scope(self):
        first = STALL.snapshot(FakeClient())
        second = STALL.snapshot(FakeClient())
        first["dirty_ram_stats"].update({"insns_run": 20, "per_pc": [
            {"pc": "0x10", "insns": 10, "hits": 1, "entries": 1}]})
        second["dirty_ram_stats"].update({"insns_run": 50, "per_pc": [
            {"pc": "0x10", "insns": 30, "hits": 2, "entries": 2}]})
        lines = "\n".join(STALL.hot_pc_lines(first, second))
        self.assertIn("accounted=20", lines)
        self.assertIn("unaccounted=10", lines)
        self.assertIn("localization=partial", lines)


if __name__ == "__main__":
    unittest.main()
