"""Source-owned disc/SBI fixtures. No retail sectors, executables or SBI records."""
import contextlib
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "tools")]
import disc_companion as dc
import prepare_disc as prepare
import psxrecomp_cli as cli
from sdk_progress import ProgressReporter

SBI = b"SBI\0" + bytes([0, 2, 0, 1]) + bytes(range(10))
BOOT = "SLES_000.00"


def cooked_disc():
    sectors = [bytearray(2048) for _ in range(24)]
    sectors[16][1:6] = b"CD001"
    struct.pack_into("<I", sectors[16], 158, 20)
    struct.pack_into("<I", sectors[16], 166, 2048)
    cursor = 0
    for name, lba, data in [("SYSTEM.CNF", 21, b"BOOT = cdrom:\\" + BOOT.encode() + b";1"),
                            (BOOT, 22, b"PS-X EXE" + bytes(2040))]:
        encoded = (name + ";1").encode()
        record = bytearray(33 + len(encoded) + (len(encoded) % 2 == 0))
        record[0] = len(record)
        struct.pack_into("<I", record, 2, lba)
        struct.pack_into("<I", record, 10, len(data))
        record[32] = len(encoded)
        record[33:33 + len(encoded)] = encoded
        sectors[20][cursor:cursor + len(record)] = record
        cursor += len(record)
        sectors[lba][:len(data)] = data
    return b"".join(sectors)


class CompanionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / "input track.bin"
        self.image.write_bytes(prepare.iso_to_bin(cooked_disc()))
        self.sha1 = hashlib.sha1(self.image.read_bytes()).hexdigest()
        self.size = self.image.stat().st_size
        self.rule = {"title": "Source fixture", "serial": "TEST-00000", "revision": "synthetic-v1",
                     "sbi_sha256": hashlib.sha256(SBI).hexdigest(), "evidence": "source-owned test"}
        self.registry = patch.dict(dc.REQUIRED_SBI, {(self.size, self.sha1): self.rule})
        self.registry.start()
        self.addCleanup(self.registry.stop)
        self.config = self.root / "game.toml"
        self.config.write_text(f'[game]\nid = "SLES-02529"\n[prepare_disc]\nboot_exe = "{BOOT}"\n', encoding="utf-8")

    def prepare(self, source=None, out=None):
        output = io.StringIO()
        args = ["prepare_disc.py", str(source or self.image), "--config", str(self.config),
                "--out-dir", str(out or self.root / "output"), "--cue-name", "renamed.cue"]
        with patch.object(sys, "argv", args), contextlib.redirect_stdout(output), contextlib.redirect_stderr(output), patch.object(prepare, "_configure_stdio"):
            code = prepare.main()
        return code, output.getvalue()

    def test_missing_blocks_before_output_even_when_main_hash_is_skipped(self):
        code, message = self.prepare()
        self.assertEqual(code, 1)
        self.assertIn("Missing SBI", message)
        self.assertIn(str(self.image.with_suffix(".sbi")), message)
        self.assertFalse((self.root / "output").exists())
        with self.assertRaisesRegex(cli.DiscVerifyError, "Missing SBI"):
            cli.verify_disc_path(self.image, {}, skip_hash=True, progress=ProgressReporter())

    def test_exact_registry_uses_measured_identity(self):
        real_size, real_hash = next(iter(dc.REQUIRED_SBI))
        with self.assertRaisesRegex(dc.CompanionError, "SLES-02529"):
            dc.inspect_companion(self.image, real_size, real_hash)
        report, data = dc.inspect_companion(self.image, real_size + 1, real_hash)
        self.assertFalse(report["required"])
        self.assertEqual(report["status"], "unknown")
        self.assertIsNone(data)

    def test_cli_missing_error_is_actionable_json_and_exit_three(self):
        output = io.StringIO()
        args = argparse.Namespace(config=str(self.config), project_root="", disc=str(self.image), skip_hash_check=True)
        reporter = ProgressReporter(json_progress=True, stream=output)
        code = cli.cmd_verify_disc(args, reporter)
        self.assertEqual(code, 3)
        error = json.loads(output.getvalue().splitlines()[-1])
        self.assertEqual(error["event"], "error")
        self.assertTrue(error["verify_failed"])
        self.assertIn("Missing SBI", error["message"])
        self.assertNotIn('"ok":true', output.getvalue())

    def test_in_place_companion_is_not_rewritten(self):
        cue = self.root / "renamed.cue"
        cue.write_text(f'FILE "{self.image.name}" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
        sbi = cue.with_suffix(".sbi")
        sbi.write_bytes(SBI)
        stamp = sbi.stat().st_mtime_ns
        code, message = self.prepare(cue, self.root)
        self.assertEqual(code, 0, message)
        self.assertEqual(sbi.stat().st_mtime_ns, stamp)
        self.assertEqual(sbi.read_bytes(), SBI)

    def test_copy_rename_receipt_and_repeat(self):
        self.image.with_suffix(".SBI").write_bytes(SBI)
        before = self.image.read_bytes()
        for _ in range(2):
            code, message = self.prepare()
            self.assertEqual(code, 0, message)
        output = self.root / "output"
        self.assertEqual((output / "renamed.sbi").read_bytes(), SBI)
        receipt = json.loads((output / "renamed.disc-receipt.json").read_text())
        self.assertEqual(receipt["subchannel"]["status"], "verified")
        self.assertEqual(receipt["subchannel"]["sha256"], self.rule["sbi_sha256"])
        self.assertEqual(receipt["source_data_track"]["sha1"], self.sha1)
        self.assertEqual(before, self.image.read_bytes())

    def test_cue_basename_multitrack_and_cdda_preserved(self):
        cue = self.root / "Album.cue"
        audio = self.root / "audio.bin"
        audio.write_bytes(bytes([42]) * 2352)
        cue.write_text(f'FILE "{self.image.name}" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\nFILE "audio.bin" BINARY\n TRACK 02 AUDIO\n INDEX 01 00:00:00\n')
        self.image.with_suffix(".sbi").write_bytes(SBI)
        code, message = self.prepare(cue)
        self.assertEqual(code, 1, "track companion must not substitute for CUE companion")
        cue.with_suffix(".sbi").write_bytes(SBI)
        code, message = self.prepare(cue)
        self.assertEqual(code, 0, message)
        output = self.root / "output"
        self.assertEqual((output / "audio.bin").read_bytes(), audio.read_bytes())
        self.assertEqual((output / "renamed.sbi").read_bytes(), SBI)
        identity = cli.verify_disc_path(cue, {}, skip_hash=False, progress=ProgressReporter())
        self.assertEqual(identity["subchannel"]["status"], "verified")

    def test_wrong_companion_and_stale_destination_are_not_overwritten(self):
        wrong = SBI[:-1] + b"\xff"
        self.image.with_suffix(".sbi").write_bytes(wrong)
        code, message = self.prepare()
        self.assertEqual(code, 1)
        self.assertIn("not qualified", message)
        self.image.with_suffix(".sbi").write_bytes(SBI)
        output = self.root / "output"
        output.mkdir()
        stale = output / "renamed.sbi"
        stale.write_bytes(wrong)
        code, message = self.prepare()
        self.assertEqual(code, 1)
        self.assertEqual(list(output.iterdir()), [stale])
        self.assertEqual(stale.read_bytes(), wrong)

    def test_optional_stale_companion_is_rejected(self):
        with patch.dict(dc.REQUIRED_SBI, {}, clear=True):
            output = self.root / "output"
            output.mkdir()
            (output / "renamed.sbi").write_bytes(SBI)
            self.assertEqual(self.prepare()[0], 1)

    def test_invalid_records(self):
        fixtures = [b"", b"SBI\0", SBI[:-1], b"BAD!" + SBI[4:],
                    SBI[:7] + b"\x02" + SBI[8:], SBI[:4] + b"\xfa" + SBI[5:],
                    SBI[:5] + b"\x60" + SBI[6:], SBI[:6] + b"\x75" + SBI[7:],
                    SBI[:4] + bytes(3) + SBI[7:], SBI + SBI[4:]]
        for data in fixtures:
            with self.subTest(data=data), self.assertRaises(dc.CompanionError):
                dc.validate_sbi(data)

    def test_unknown_iso_and_raw_conversion_preserve_supplied_bytes(self):
        for kind in ["iso", "raw"]:
            source = self.root / ("cooked.iso" if kind == "iso" else "raw.bin")
            data = cooked_disc() if kind == "iso" else b"".join(
                self.image.read_bytes()[i:i + 2352] + bytes(96)
                for i in range(0, self.size, 2352))
            source.write_bytes(data)
            source.with_suffix(".sbi").write_bytes(SBI)
            out = self.root / kind
            code, message = self.prepare(source, out)
            self.assertEqual(code, 0, message)
            self.assertEqual((out / "renamed.sbi").read_bytes(), SBI)
            report = json.loads((out / "renamed.disc-receipt.json").read_text())
            self.assertEqual(report["subchannel"]["status"], "format_valid_unqualified")

    def test_unknown_serial_is_not_protection_detection_and_cli_subprocess(self):
        source = self.root / "unknown.bin"
        source.write_bytes(self.image.read_bytes() + bytes(2352))
        code, message = self.prepare(source)
        self.assertEqual(code, 0, message)
        self.assertIn("subchannel: unknown", message)
        proc = subprocess.run([sys.executable, str(ROOT / "psxrecomp_cli.py"),
                               "verify-disc", "--config", str(self.config), "--disc", str(source),
                               "--json-progress"], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        events = [json.loads(line) for line in proc.stdout.splitlines()]
        self.assertEqual(events[-1]["subchannel"]["status"], "unknown")


if __name__ == "__main__":
    unittest.main()
