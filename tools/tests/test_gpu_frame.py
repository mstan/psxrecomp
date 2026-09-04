#!/usr/bin/env python3
"""Tests for psx_gpu_frame decode + gpu_frame_layers rendering.

These pin the parts that are easy to get subtly wrong and impossible to notice
downstream: packet word layouts, the 11-bit signed vertex, the GP0(E5) draw
offset, the texpage latch a textured polygon performs on draw-mode state, and
the four semi-transparency blends.
"""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


GF = _load("psx_gpu_frame")
LAYERS = _load("gpu_frame_layers")
DIFF = _load("gpu_frame_diff")


def entry(seq, op, words, func="0x80012340", ot=0, ra="0x80011000"):
    return {
        "seq": seq,
        "op": f"0x{op:02X}",
        "n": len(words),
        "src": "0x80100000",
        "ot": ot,
        "pc": "0x80012348",
        "func": func,
        "ra": ra,
        "w": [f"0x{w:08X}" for w in words],
    }


def vert(x, y):
    return (y & 0x7FF) << 16 | (x & 0x7FF)


def color(r, g, b):
    return (b << 16) | (g << 8) | r


class TestWordLayouts(unittest.TestCase):
    def test_poly_word_counts_match_hardware(self):
        # Values cross-checked against gpu.c's gp0_exec_* command lengths.
        self.assertEqual(GF.poly_words(0x20), 4)    # PolyF3
        self.assertEqual(GF.poly_words(0x24), 7)    # PolyFT3
        self.assertEqual(GF.poly_words(0x28), 5)    # PolyF4
        self.assertEqual(GF.poly_words(0x2C), 9)    # PolyFT4
        self.assertEqual(GF.poly_words(0x30), 6)    # PolyG3
        self.assertEqual(GF.poly_words(0x34), 9)    # PolyGT3
        self.assertEqual(GF.poly_words(0x38), 8)    # PolyG4
        self.assertEqual(GF.poly_words(0x3C), 12)   # PolyGT4

    def test_rect_word_counts(self):
        self.assertEqual(GF.rect_words(0x60), 3)    # variable, flat
        self.assertEqual(GF.rect_words(0x64), 4)    # variable, textured
        self.assertEqual(GF.rect_words(0x68), 2)    # 1x1
        self.assertEqual(GF.rect_words(0x74), 3)    # 8x8 textured

    def test_vertex_is_signed_11_bit(self):
        self.assertEqual(GF.parse_vertex(vert(16, 32)), (16, 32))
        self.assertEqual(GF.parse_vertex(vert(-16, -32)), (-16, -32))
        self.assertEqual(GF.parse_vertex(vert(0x400, 0)), (-1024, 0))


class TestDecode(unittest.TestCase):
    def test_flat_triangle(self):
        e = entry(0, 0x20, [color(10, 20, 30), vert(0, 0), vert(10, 0), vert(0, 10)])
        p = GF.decode_entries([e])[0]
        self.assertEqual(p["kind"], "poly")
        self.assertEqual(p["op_name"], "PolyF3")
        self.assertEqual(p["verts"], [[0, 0], [10, 0], [0, 10]])
        self.assertEqual(p["colors"], [[10, 20, 30]] * 3)
        self.assertFalse(p["semi"])

    def test_gouraud_quad_keeps_per_vertex_colour(self):
        words = [color(1, 1, 1), vert(0, 0),
                 color(2, 2, 2), vert(8, 0),
                 color(3, 3, 3), vert(0, 8),
                 color(4, 4, 4), vert(8, 8)]
        p = GF.decode_entries([entry(0, 0x38, words)])[0]
        self.assertEqual(len(p["verts"]), 4)
        self.assertEqual(p["colors"], [[1, 1, 1], [2, 2, 2], [3, 3, 3], [4, 4, 4]])

    def test_draw_offset_is_applied(self):
        off = (7 & 0x7FF) | ((11 & 0x7FF) << 11)
        stream = [entry(0, 0xE5, [off]),
                  entry(1, 0x20, [color(0, 0, 0), vert(1, 2), vert(3, 4), vert(5, 6)])]
        prims = GF.decode_entries(stream)
        self.assertEqual(prims[1]["verts"], [[8, 13], [10, 15], [12, 17]])

    def test_textured_poly_latches_tpage_for_later_prims(self):
        # A PolyFT3 whose own tpage word selects STP mode 2 (B-F). The
        # rectangle that follows carries no tpage word and must inherit it --
        # this is set_tpage_from_poly() in gpu.c.
        tpage = (2 << 5)  # semi-transparency bits 5-6 = 2
        words = [color(128, 128, 128), vert(0, 0),
                 0x0000_0000, vert(8, 0),
                 (tpage << 16), vert(0, 8), 0x0000_0000]
        stream = [entry(0, 0x24, words),
                  entry(1, 0x62, [color(9, 9, 9), vert(4, 4), (6 << 16) | 5])]
        prims = GF.decode_entries(stream)
        self.assertEqual(prims[0]["tpage"], tpage)
        self.assertEqual(prims[0]["stp"], 2)
        self.assertEqual(prims[1]["stp"], 2, "rect must inherit the poly's latched mode")
        self.assertEqual(prims[1]["semi"], True)
        self.assertEqual(prims[1]["size"], [5, 6])

    def test_e1_sets_mode_for_untextured_poly(self):
        stream = [entry(0, 0xE1, [(1 << 5)]),
                  entry(1, 0x22, [color(0, 0, 0), vert(0, 0), vert(4, 0), vert(0, 4)])]
        prims = GF.decode_entries(stream)
        self.assertEqual(prims[1]["stp"], 1)
        self.assertTrue(prims[1]["semi"])

    def test_truncated_packet_is_flagged_not_guessed(self):
        e = entry(0, 0x3C, [color(1, 1, 1), vert(0, 0)])
        e["n"] = 12
        p = GF.decode_entries([e])[0]
        self.assertTrue(p["truncated"])
        self.assertEqual(len(p["verts"]), 1)


class TestAttribution(unittest.TestCase):
    def test_groups_by_func_and_counts_semi(self):
        stream = [
            entry(0, 0x20, [color(1, 1, 1), vert(0, 0), vert(4, 0), vert(0, 4)],
                  func="0x80001000"),
            entry(1, 0x22, [color(1, 1, 1), vert(0, 0), vert(4, 0), vert(0, 4)],
                  func="0x80001000", ot=5),
            entry(2, 0x20, [color(1, 1, 1), vert(0, 0), vert(4, 0), vert(0, 4)],
                  func="0x80002000", ot=9),
        ]
        funcs = GF.attribute(GF.decode_entries(stream))
        self.assertEqual(funcs["0x80001000"]["drawing"], 2)
        self.assertEqual(funcs["0x80001000"]["semi"], 1)
        self.assertEqual(funcs["0x80002000"]["ot_min"], 9)
        self.assertIn("0.5B+0.5F", funcs["0x80001000"]["stp_modes"])


class TestBlends(unittest.TestCase):
    def _one(self, semi, stp, base=100.0):
        c = LAYERS.Canvas(0, 0, 4, 4)
        c.rgb[:] = base
        c.rect(0, 0, 4, 4, (40.0, 40.0, 40.0), semi, stp)
        return float(c.rgb[0, 0, 0])

    def test_four_modes(self):
        self.assertEqual(self._one(False, 0), 40.0)     # opaque
        self.assertEqual(self._one(True, 0), 70.0)      # 0.5B + 0.5F
        self.assertEqual(self._one(True, 1), 140.0)     # B + F
        self.assertEqual(self._one(True, 2), 60.0)      # B - F
        self.assertEqual(self._one(True, 3), 110.0)     # B + 0.25F

    def test_subtractive_clamps_at_zero(self):
        c = LAYERS.Canvas(0, 0, 2, 2)
        c.rgb[:] = 10.0
        c.rect(0, 0, 2, 2, (200.0, 200.0, 200.0), True, 2)
        self.assertEqual(float(c.rgb[0, 0, 0]), 0.0)


class TestRender(unittest.TestCase):
    def _dump(self):
        stream = [
            entry(0, 0xE3, [0]),
            entry(1, 0xE4, [(31 << 10) | 31]),
            entry(2, 0x20, [color(255, 0, 0), vert(0, 0), vert(31, 0), vert(0, 31)],
                  func="0x80001000"),
            entry(3, 0x62, [color(0, 0, 255), vert(4, 4), (8 << 16) | 8],
                  func="0x80002000"),
        ]
        prims = GF.decode_entries(stream)
        return {"kind": GF.DUMP_KIND, "version": 1, "frame": 7, "label": "t",
                "raw_count": len(prims), "prims": prims, "funcs": GF.attribute(prims)}

    def test_renders_one_layer_per_function(self):
        with tempfile.TemporaryDirectory() as td:
            idx = LAYERS.render(self._dump(), td)
            files = {l["func"]: l["file"] for l in idx["layers"]}
            self.assertEqual(set(files), {"0x80001000", "0x80002000"})
            for f in files.values():
                self.assertTrue((Path(td) / f).is_file())
            self.assertTrue((Path(td) / "composite.png").is_file())
            self.assertTrue((Path(td) / "sheet.png").is_file())
            written = json.loads((Path(td) / "layers.json").read_text())
            self.assertEqual(written["frame"], 7)
            rect = next(l for l in written["layers"] if l["func"] == "0x80002000")
            self.assertEqual(rect["pixels"], 64)
            self.assertEqual(rect["bbox"], [4, 4, 11, 11])

    def test_draw_area_from_e3_e4(self):
        self.assertEqual(GF.draw_area(self._dump()), (0, 0, 32, 32))


def _dump_of(stream, label, frame):
    prims = GF.decode_entries(stream)
    return {"kind": GF.DUMP_KIND, "version": 1, "frame": frame, "label": label,
            "raw_count": len(prims), "prims": prims, "funcs": GF.attribute(prims)}


class TestDiff(unittest.TestCase):
    """The scenario this whole toolchain was built for: an effect where the
    semi-transparent layer stops being drawn and the opaque one keeps going."""

    RAYS = "0x80041000"
    VIGNETTE = "0x80042000"

    def _good(self):
        stream = []
        for i in range(6):
            stream.append(entry(i, 0x32,  # PolyF3 + semi -> the glow rays
                                [color(200, 60, 40), vert(0, 0), vert(31, i),
                                 vert(i, 31)], func=self.RAYS, ot=20))
        # The vignette: a B-F subtractive full-screen quad. Mode 2 comes from
        # the E1 that precedes it, which is exactly how the game would do it.
        stream.append(entry(6, 0xE1, [(2 << 5)]))
        stream.append(entry(7, 0x2A, [color(90, 90, 90), vert(0, 0), vert(31, 0),
                                      vert(0, 31), vert(31, 31)],
                            func=self.VIGNETTE, ot=1))
        return _dump_of(stream, "good", 100)

    def _bad(self):
        # Same rays, but the STP bit is gone (opcode 0x30 not 0x32) and the
        # vignette function never emitted anything at all.
        stream = []
        for i in range(6):
            stream.append(entry(i, 0x30,
                                [color(200, 60, 40), vert(0, 0), vert(31, i),
                                 vert(i, 31), vert(0, 0), vert(0, 0)],
                                func=self.RAYS, ot=20))
        return _dump_of(stream, "bad", 140)

    def test_names_the_function_that_stopped_drawing(self):
        d = DIFF.diff(self._good(), self._bad())
        by_func = {f["func"]: f for f in d["funcs"]}
        self.assertEqual(by_func[self.VIGNETTE]["verdict"], "stopped drawing")
        self.assertEqual(by_func[self.VIGNETTE]["a"]["prims"], 1)
        self.assertEqual(by_func[self.VIGNETTE]["b"]["prims"], 0)

    def test_names_the_function_that_lost_its_blend(self):
        d = DIFF.diff(self._good(), self._bad())
        by_func = {f["func"]: f for f in d["funcs"]}
        self.assertEqual(by_func[self.RAYS]["verdict"], "lost semi-transparency")
        self.assertEqual(by_func[self.RAYS]["a"]["semi"], 6)
        self.assertEqual(by_func[self.RAYS]["b"]["semi"], 0)

    def test_reports_the_blend_modes_that_disappeared(self):
        d = DIFF.diff(self._good(), self._bad())
        self.assertEqual(d["modes_a"].get("B-F"), 1)
        self.assertEqual(d["modes_a"].get("0.5B+0.5F"), 6)
        self.assertEqual(d["modes_b"], {})

    def test_report_headlines_both_findings(self):
        import io
        buf = io.StringIO()
        DIFF.report(DIFF.diff(self._good(), self._bad()), out=buf)
        text = buf.getvalue()
        self.assertIn("B-F", text)
        self.assertIn(self.VIGNETTE, text)
        self.assertIn("stopped drawing", text)

    def test_identical_frames_report_no_findings(self):
        import io
        buf = io.StringIO()
        DIFF.report(DIFF.diff(self._good(), self._good()), out=buf)
        self.assertIn("No function stopped drawing", buf.getvalue())


class OneShotServer:
    """Mimics runtime/src/debug_server.c io_thread_main(): accept, read one
    line, reply, close. Any client that assumes a persistent socket fails
    against this exactly the way it fails against the real runtime."""

    def __init__(self, handler):
        import socket as _s
        import threading
        self.handler = handler
        self.requests = []
        self.sock = _s.socket(_s.AF_INET, _s.SOCK_STREAM)
        self.sock.setsockopt(_s.SOL_SOCKET, _s.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while self.running:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            try:
                buf = b""
                while b"\n" not in buf:
                    chunk = c.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                req = json.loads(buf.split(b"\n")[0].decode())
                self.requests.append(req)
                reply = self.handler(req)
                reply.setdefault("id", req.get("id", 0))
                c.sendall((json.dumps(reply) + "\n").encode())
            except Exception:
                pass
            finally:
                c.close()   # <- the contract: one request per connection

    def stop(self):
        self.running = False
        self.sock.close()


class TestDebugConn(unittest.TestCase):
    def test_multiple_commands_survive_a_one_shot_server(self):
        stream = [entry(0, 0x20, [color(7, 7, 7), vert(0, 0), vert(9, 0), vert(0, 9)],
                        func="0x80050000")]

        def handler(req):
            if req["cmd"] == "ping":
                return {"ok": True, "frame": 4242}
            if req["cmd"] == "gpu_frame_dump":
                return {"ok": True, "frame": req["frame"], "count": len(stream),
                        "max_words": 12, "entries": stream}
            return {"ok": False, "error": "unknown"}

        srv = OneShotServer(handler)
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        # Three commands over what would be three separate sockets. A client
        # that reuses one socket hangs here instead.
        self.assertEqual(conn.frame(), 4242)
        self.assertEqual(conn.frame(), 4242)
        dump = GF.capture(conn, frame=4242, label="t")
        self.assertEqual(dump["frame"], 4242)
        self.assertEqual(dump["raw_count"], 1)
        self.assertIn("0x80050000", dump["funcs"])
        # ping, ping, gpu_ring_stats (declined here), gpu_frame_dump -- four
        # commands, and therefore four separate connections. The count is the
        # assertion: a client reusing one socket would never get this far.
        self.assertEqual([r["cmd"] for r in srv.requests],
                         ["ping", "ping", "gpu_ring_stats", "gpu_frame_dump"])

    def test_error_replies_raise_with_the_server_message(self):
        srv = OneShotServer(lambda req: {"ok": False, "error": "missing frame"})
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        with self.assertRaises(GF.DebugError) as cm:
            conn.cmd("gpu_frame_dump", frame=1)
        self.assertIn("missing frame", str(cm.exception))

    def test_capture_uses_the_ring_span_when_no_frame_is_given(self):
        stream = [entry(0, 0x20, [color(1, 1, 1), vert(0, 0), vert(4, 0), vert(0, 4)],
                        func="0x80060000")]

        def handler(req):
            if req["cmd"] == "gpu_ring_stats":
                return {"ok": True, "total": 5000, "capacity": 1 << 20,
                        "max_words": 12, "oldest_frame": 900, "newest_frame": 1000}
            if req["cmd"] == "gpu_frame_dump":
                return {"ok": True, "frame": req["frame"], "count": len(stream),
                        "max_words": 12, "entries": stream}
            if req["cmd"] == "ping":
                return {"ok": True, "frame": 1234}
            return {"ok": False, "error": "unknown"}

        srv = OneShotServer(handler)
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        self.assertEqual(conn.ring_span()["newest"], 1000)
        # No --frame: take the newest the RING holds, not whatever ping says the
        # runtime is on. Those differ, and only the ring one is dumpable.
        dump = GF.capture(conn, label="t")
        self.assertEqual(dump["frame"], 1000)
        self.assertEqual(dump["ring"]["oldest"], 900)

    def test_capture_refuses_a_frame_that_fell_out_of_the_ring(self):
        def handler(req):
            if req["cmd"] == "gpu_ring_stats":
                return {"ok": True, "total": 5000, "capacity": 1 << 20,
                        "max_words": 12, "oldest_frame": 900, "newest_frame": 1000}
            return {"ok": True, "frame": 1, "count": 0, "entries": []}

        srv = OneShotServer(handler)
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        with self.assertRaises(GF.DebugError) as cm:
            GF.capture(conn, frame=42, label="t")
        msg = str(cm.exception)
        self.assertIn("900..1000", msg)
        # An evicted frame must not be reported as a frame that drew nothing.
        self.assertIn("empty, not zero-drawn", msg)

    def test_capture_rejects_a_server_that_answers_ok_with_no_entries(self):
        # Exactly what a leftover stub does: {"ok": true} and nothing else. The
        # old code turned that into a cheerful 0-packet dump.
        srv = OneShotServer(lambda req: {"ok": True})
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        with self.assertRaises(GF.DebugError) as cm:
            GF.capture(conn, frame=5, label="t")
        self.assertIn("not a psx-runtime debug server", str(cm.exception))

    def test_capture_survives_a_server_without_gpu_ring_stats(self):
        stream = [entry(0, 0x20, [color(1, 1, 1), vert(0, 0), vert(4, 0), vert(0, 4)],
                        func="0x80060000")]

        def handler(req):
            if req["cmd"] == "gpu_ring_stats":
                return {"ok": False, "error": "unknown command"}
            if req["cmd"] == "ping":
                return {"ok": True, "frame": 77}
            return {"ok": True, "frame": req.get("frame", 0), "count": 1,
                    "entries": stream}

        srv = OneShotServer(handler)
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        dump = GF.capture(conn, label="t")
        self.assertEqual(dump["frame"], 77)      # fell back to ping
        self.assertIsNone(dump["ring"])

    def test_removed_transport_commands_raise_the_servers_own_message(self):
        # psx-runtime keeps pause/step/run_to_frame registered as error stubs.
        # The tools must surface that text, not swallow it into a silent no-op.
        removed = ("pause is removed; query a ring buffer",
                   "run_to_frame is removed; use frame_range")

        def handler(req):
            if req["cmd"] == "pause":
                return {"ok": False, "error": removed[0]}
            if req["cmd"] == "run_to_frame":
                return {"ok": False, "error": removed[1]}
            return {"ok": True}

        srv = OneShotServer(handler)
        self.addCleanup(srv.stop)
        conn = GF.DebugConn("127.0.0.1", srv.port, timeout=5.0)
        with self.assertRaises(GF.DebugError) as cm:
            conn.run_to_frame(10)
        self.assertIn("frame_range", str(cm.exception))
        with self.assertRaises(GF.DebugError) as cm2:
            conn.cmd("pause")
        self.assertIn("ring buffer", str(cm2.exception))

    def test_summary_round_trips(self):
        stream = [entry(0, 0x22, [color(7, 7, 7), vert(0, 0), vert(9, 0), vert(0, 9)],
                        func="0x80050000")]
        prims = GF.decode_entries(stream)
        dump = {"kind": GF.DUMP_KIND, "version": 1, "frame": 5, "label": "x",
                "raw_count": 1, "prims": prims, "funcs": GF.attribute(prims)}
        with tempfile.TemporaryDirectory() as td:
            path = str(Path(td) / "x.summary.json")
            s = GF.save_summary(dump, path, dump_name="x.json")
            again = json.loads(Path(path).read_text())
            self.assertEqual(again["kind"], GF.SUMMARY_KIND)
            self.assertEqual(again["drawing"], 1)
            self.assertEqual(again["modes"], {"0.5B+0.5F": 1})
            self.assertEqual(s["funcs"][0]["func"], "0x80050000")


if __name__ == "__main__":
    unittest.main()
