#!/usr/bin/env python3
"""The overlay producer must DECLARE its trailing delay-slot guard words.

overlay_capture.c appends one coherent guard instruction past the end of a
dirty-page run so a MIPS branch at the run's final word (...FFC) has its
architectural delay slot (...000). The recompiler must be TOLD how many
trailing bytes that is, because the guard word is a legal delay-slot source
and an illegal block leader -- the word after it does not exist in the image.
Inferring the count inside the recompiler from `size % 4096 == 4` would fit
every capture written by the current format and would silently shift
underneath any future change to capture granularity.

This pins the whole producer chain (bead beads-eio.3.100):

  overlay_capture.c  writes "guard_bytes"           (writer states intent)
      -> compile_overlays.capture_guard_bytes       (prefer it; reconstruct
                                                     for pre-field captures)
      -> compile_overlays.make_psxexe               (tagged PS-EXE header)
      -> ps1_exe_parser.h exe_tag                   (recompiler reads it)
"""

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import compile_overlays  # noqa: E402


class GuardByteDerivationTests(unittest.TestCase):
    def test_explicit_field_wins(self):
        self.assertEqual(
            compile_overlays.capture_guard_bytes({"guard_bytes": 4}, 0x2004), 4)
        # A page-multiple+4 capture that explicitly says it has NO guard word
        # must be believed: the writer knows, the size does not.
        self.assertEqual(
            compile_overlays.capture_guard_bytes({"guard_bytes": 0}, 0x2004), 0)

    def test_legacy_capture_reconstructs_from_the_page_run_format(self):
        # Current format: whole 4 KiB pages plus one guard word.
        self.assertEqual(compile_overlays.capture_guard_bytes({}, 0x2004), 4)
        self.assertEqual(compile_overlays.capture_guard_bytes({}, 0x1004), 4)
        # Pre-2026-07-25 captures are page-exact and have no guard word.
        self.assertEqual(compile_overlays.capture_guard_bytes({}, 0x2000), 0)
        self.assertEqual(compile_overlays.capture_guard_bytes({}, 0x1000), 0)

    def test_unknown_residue_falls_back_to_zero(self):
        # Not this tool's format. Zero is the fail-closed answer: the emitter's
        # mandatory-delay-slot check still refuses a transfer it cannot finish.
        self.assertEqual(compile_overlays.capture_guard_bytes({}, 0x2008), 0)
        self.assertEqual(compile_overlays.capture_guard_bytes({}, 100), 0)

    def test_nonsense_declaration_is_rejected(self):
        self.assertEqual(
            compile_overlays.capture_guard_bytes({"guard_bytes": 7}, 0x2004), 0)
        self.assertEqual(
            compile_overlays.capture_guard_bytes(
                {"guard_bytes": "four"}, 0x2004), 0)
        # A declaration that would consume the whole image is not a trailer.
        self.assertEqual(
            compile_overlays.capture_guard_bytes({"guard_bytes": 4}, 4), 0)


class PsxExeTagTests(unittest.TestCase):
    PAYLOAD = b"\x00" * 0x2000 + struct.pack("<I", 0x1062005A)

    def test_tag_lands_where_the_recompiler_reads_it(self):
        wrapped = compile_overlays.make_psxexe(
            0x80136000, 0x80136000, self.PAYLOAD, guard_bytes=4)
        self.assertEqual(wrapped[:8], b"PS-X EXE")
        self.assertEqual(len(wrapped), 2048 + len(self.PAYLOAD))
        off = compile_overlays.GUARD_TAG_MAGIC_OFFSET
        self.assertEqual(wrapped[off:off + 8], b"PSXRGRD1")
        self.assertEqual(
            struct.unpack_from(
                "<I", wrapped, compile_overlays.GUARD_TAG_COUNT_OFFSET)[0], 4)
        # The tag must live strictly inside the 2048-byte header, past every
        # field a real PS-X EXE uses.
        self.assertGreater(compile_overlays.GUARD_TAG_MAGIC_OFFSET, 0x100)
        self.assertLessEqual(
            compile_overlays.GUARD_TAG_COUNT_OFFSET + 4, 2048)

    def test_no_tag_when_there_is_no_guard_word(self):
        wrapped = compile_overlays.make_psxexe(
            0x80010000, 0x80010000, self.PAYLOAD, guard_bytes=0)
        self.assertEqual(wrapped[2048:], self.PAYLOAD)
        self.assertNotIn(b"PSXRGRD1", wrapped[:2048])
        # A guard_bytes=0 wrap must be byte-identical to the pre-fix wrapper's
        # output, so every image without a guard word analyses exactly as
        # before. Rebuild that header here rather than trusting the absence of
        # the magic alone.
        expected = bytearray(2048)
        expected[0:8] = b"PS-X EXE"
        struct.pack_into("<I", expected, 0x10, 0x80010000)
        struct.pack_into("<I", expected, 0x14, 0)
        struct.pack_into("<I", expected, 0x18, 0x80010000)
        struct.pack_into("<I", expected, 0x1C, len(self.PAYLOAD))
        self.assertEqual(wrapped[:2048], bytes(expected))

    def test_guard_bytes_is_mandatory_and_validated(self):
        with self.assertRaises(TypeError):
            compile_overlays.make_psxexe(0x80010000, 0x80010000, self.PAYLOAD)
        for bad in (-4, 3, len(self.PAYLOAD)):
            with self.assertRaises(ValueError):
                compile_overlays.make_psxexe(
                    0x80010000, 0x80010000, self.PAYLOAD, guard_bytes=bad)


class CaptureWriterTests(unittest.TestCase):
    SOURCE = (ROOT / "runtime" / "src" / "overlay_capture.c").read_text(
        encoding="utf-8")

    def test_writer_still_appends_the_guard_word(self):
        self.assertIn("size += 4u", self.SOURCE)
        self.assertIn("phys + size <= ram_size - 4u", self.SOURCE)

    def test_writer_declares_what_it_appended(self):
        self.assertIn("guard_bytes = 4u", self.SOURCE)
        self.assertIn('\\"guard_bytes\\": %u', self.SOURCE)


if __name__ == "__main__":
    unittest.main()
