#!/usr/bin/env python3
import importlib.util
import json
import os
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "coverage_vault", ROOT / "tools" / "coverage_vault.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)


class CoverageVaultHistoryTests(unittest.TestCase):
    def test_capture_merge_preserves_static_dispatch_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            vault = os.path.join(tmp, 'captures.json')
            base = {
                'load_addr': '0x80010000', 'size': 4,
                'bytes_b64': 'AAAAAA==',
                'dispatch_entry_pcs': ['0x80010000'],
            }
            with open(vault, 'w', encoding='utf-8') as out:
                json.dump([base], out)
            enriched = dict(
                base,
                dispatch_entry_pcs=['0x80010000', '0x80010004'],
                static_dispatch_entry_pcs=['0x80010004'])
            self.assertEqual(
                MOD.merge_capture_regions(vault, [enriched]), (0, 0))
            with open(vault, encoding='utf-8') as source:
                merged = json.load(source)[0]
            self.assertEqual(merged['dispatch_entry_pcs'],
                             ['0x80010000', '0x80010004'])
            self.assertEqual(merged['static_dispatch_entry_pcs'],
                             ['0x80010004'])

    def test_cache_merge_preserves_resident_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, 'source')
            dst = os.path.join(tmp, 'vault')
            rel = os.path.join('gcc', 'win-x64', 'cg5_12345678')
            os.makedirs(os.path.join(src, rel))
            stem = os.path.join(src, rel, '0000DF80_4EE6AC69')
            for ext, contents in (('.dll', b'dll'), ('.ranges', b'ranges'),
                                  ('.resident', b'resident')):
                with open(stem + ext, 'wb') as out:
                    out.write(contents)
            self.assertEqual(MOD.merge_cache(dst, src), 1)
            for ext in ('.dll', '.ranges', '.resident'):
                self.assertTrue(os.path.exists(os.path.join(
                    dst, rel, '0000DF80_4EE6AC69' + ext)))

    def test_v1_and_verified_v2_records_survive_a_torn_tail(self):
        with tempfile.TemporaryDirectory() as tmp:
            snapshot = os.path.join(tmp, "immutable.json")
            addendum = os.path.join(tmp, "history.jsonl")
            v1_region = {"load_addr": "0x80010000", "bytes_b64": "AA=="}
            v2_region = {"load_addr": "0x80100000", "bytes_b64": "AQ=="}
            with open(snapshot, "w", encoding="utf-8", newline="\n") as out:
                json.dump([v2_region], out)
            signature = "%016X" % MOD._fnv64_file(snapshot)
            records = [
                {"schema": "psxrecomp overlay capture addendum v1",
                 "captures": [v1_region]},
                {"schema": "psxrecomp overlay capture addendum v2",
                 "snapshot": snapshot, "fnv64": signature},
                # A repeated reference is idempotent and should not inflate RAM.
                {"schema": "psxrecomp overlay capture addendum v2",
                 "snapshot": snapshot, "fnv64": signature},
                # A bad signature must fail closed instead of ingesting corruption.
                {"schema": "psxrecomp overlay capture addendum v2",
                 "snapshot": snapshot, "fnv64": "0000000000000000"},
            ]
            with open(addendum, "w", encoding="utf-8", newline="\n") as out:
                for record in records:
                    out.write(json.dumps(record) + "\n")
                out.write('{"schema":"torn')
            self.assertEqual(MOD._load_addendum(addendum),
                             [v1_region, v2_region])

    def test_compact_addendum_converts_v1_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            persist = os.path.join(tmp, "immutable")
            os.mkdir(persist)
            addendum = os.path.join(tmp, "history.jsonl")
            snapshot = os.path.join(
                persist, "GAME_session_0001_0000000000000000.json")
            region = {"load_addr": "0x80010000", "bytes_b64": "AA=="}
            with open(snapshot, "w", encoding="utf-8", newline="\n") as out:
                json.dump([region], out)
            signature = "%016X" % MOD._fnv64_file(snapshot)
            corrected = os.path.join(
                persist, "GAME_session_0001_%s.json" % signature)
            os.replace(snapshot, corrected)
            v1 = {"schema": "psxrecomp overlay capture addendum v1",
                  "game": "GAME", "session": "session", "sequence": 1,
                  "reason": "autocap", "fnv64": signature,
                  "captures": [region]}
            with open(addendum, "w", encoding="utf-8", newline="\n") as out:
                out.write(json.dumps(v1) + "\n")
                out.write('{"schema":"torn')
            self.assertEqual(MOD.compact_addendum(addendum, persist),
                             (1, 0, 0, 1))
            with open(addendum, encoding="utf-8") as source:
                compacted = json.loads(source.readline())
            self.assertNotIn("captures", compacted)
            self.assertEqual(compacted["snapshot"], corrected)
            self.assertEqual(MOD._load_addendum(addendum), [region])
            self.assertEqual(MOD.compact_addendum(addendum, persist),
                             (0, 1, 0, 0))
            self.assertEqual(MOD._load_addendum(addendum), [region])

    def test_compact_addendum_failure_preserves_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            persist = os.path.join(tmp, "immutable")
            os.mkdir(persist)
            addendum = os.path.join(tmp, "history.jsonl")
            v1 = {"schema": "psxrecomp overlay capture addendum v1",
                  "game": "GAME", "session": "missing", "sequence": 1,
                  "reason": "autocap", "fnv64": "1234567890ABCDEF",
                  "captures": []}
            original = (json.dumps(v1) + "\n").encode()
            with open(addendum, "wb") as out:
                out.write(original)
            with self.assertRaisesRegex(ValueError, "snapshot missing"):
                MOD.compact_addendum(addendum, persist)
            with open(addendum, "rb") as source:
                self.assertEqual(source.read(), original)


if __name__ == "__main__":
    unittest.main()
