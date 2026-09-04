#!/usr/bin/env python3
import importlib.util
import base64
import json
import os
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "coverage_vault", ROOT / "tools" / "coverage_vault.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)


class CoverageVaultHistoryTests(unittest.TestCase):
    def test_streaming_array_reader_handles_chunk_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = os.path.join(tmp, "large.json")
            records = [{"value": "x" * 100}, {"value": "y" * 137}]
            with open(source, "w", encoding="utf-8") as out:
                json.dump(records, out)
            self.assertEqual(list(MOD._iter_json_array(source, chunk_size=7)),
                             records)

    def test_compaction_splits_execution_pages_and_collapses_mutable_gap(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = os.path.join(tmp, "overlay_captures.json")
            output = os.path.join(tmp, "compacted.json")
            first = bytearray(3 * 4096)
            second = bytearray(first)
            first[0:4] = second[0:4] = b"CODE"
            first[8192:8196] = second[8192:8196] = b"TAIL"
            first[4096 + 100] = 0x11
            second[4096 + 100] = 0x22  # mutable, never-executed middle page
            records = []
            for image, dispatch in ((first, "0x80010000"),
                                    (second, "0x80012000")):
                records.append({
                    "schema": "psxrecomp overlay capture v2",
                    "load_addr": "0x80010000", "size": len(image),
                    "bytes_b64": base64.b64encode(image).decode("ascii"),
                    "executed_pcs": ["0x80010000", "0x80012000"],
                    "dispatch_entry_pcs": [dispatch],
                    "function_entry_pcs": [], "seeds": [dispatch],
                })
            with open(source, "w", encoding="utf-8") as out:
                json.dump(records, out)
            compacted, stats = MOD.compact_capture_manifest(source, output)
            self.assertEqual(stats["source_regions"], 2)
            self.assertEqual(stats["output_variants"], 2)
            self.assertEqual(stats["max_output_region"], 4100)
            self.assertEqual([r["load_addr"] for r in compacted],
                             ["0x80010000", "0x80012000"])
            self.assertEqual(compacted[0]["size"], 4100)
            self.assertEqual(compacted[1]["size"], 4096)
            self.assertEqual(compacted[0]["dispatch_entry_pcs"],
                             ["0x80010000"])
            self.assertEqual(compacted[1]["dispatch_entry_pcs"],
                             ["0x80012000"])
            self.assertEqual(base64.b64decode(compacted[0]["bytes_b64"])[:4],
                             b"CODE")
            self.assertEqual(base64.b64decode(compacted[1]["bytes_b64"])[:4],
                             b"TAIL")
            with open(output, encoding="utf-8") as saved:
                self.assertEqual(json.load(saved), compacted)

    def test_compaction_fails_closed_on_out_of_region_evidence(self):
        image = bytes(4096)
        region = {
            "load_addr": "0x80010000", "size": len(image),
            "bytes_b64": base64.b64encode(image).decode("ascii"),
            "executed_pcs": ["0x80012000"],
        }
        with self.assertRaisesRegex(ValueError, "outside/alignment-invalid"):
            MOD._compact_region(region)
        compacted, evidence, invalid = MOD._compact_region(
            region, drop_invalid_evidence=True)
        self.assertEqual(compacted, [])
        self.assertEqual(evidence, 0)
        self.assertEqual(invalid, 1)

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
