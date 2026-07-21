import base64
import json
import tempfile
import unittest
from unittest import mock
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays
import coverage_vault


def region(load_addr, payload, executed=(), dispatch=(), functions=(), seeds=None):
    if seeds is None:
        seeds = dispatch
    return {
        "schema": "psxrecomp overlay capture v2",
        "load_addr": f"0x{load_addr:08X}",
        "size": len(payload),
        "bytes_b64": base64.b64encode(payload).decode("ascii"),
        "executed_pcs": [f"0x{x:08X}" for x in executed],
        "dispatch_entry_pcs": [f"0x{x:08X}" for x in dispatch],
        "function_entry_pcs": [f"0x{x:08X}" for x in functions],
        "seeds": [f"0x{x:08X}" for x in seeds],
    }


class AdditiveCaptureTests(unittest.TestCase):
    def test_cached_bundle_pairs_preserves_logical_key_for_immutable_names(self):
        with tempfile.TemporaryDirectory() as td:
            logical = Path(td) / "00010000_DEADBEEF.dll"
            legacy_ranges = logical.with_suffix(".ranges")
            logical.write_bytes(b"legacy")
            legacy_ranges.write_text("legacy", encoding="ascii")

            immutable = Path(td) / "00010000_DEADBEEF_13579BDF.dll"
            immutable_ranges = immutable.with_suffix(".ranges")
            immutable.write_bytes(b"immutable")
            immutable_ranges.write_text("immutable", encoding="ascii")

            # A different logical key must not be treated as the same cache
            # identity merely because it shares the region prefix.
            other = Path(td) / "00010000_FEEDFACE_2468ACE0.dll"
            other.write_bytes(b"other")
            other.with_suffix(".ranges").write_text("other", encoding="ascii")

            pairs = set(compile_overlays.cached_bundle_pairs(str(logical)))
            self.assertEqual(pairs, {
                (str(logical), str(legacy_ranges)),
                (str(immutable), str(immutable_ranges)),
            })

    def test_validated_same_logical_artifacts_union_additive_entries(self):
        with tempfile.TemporaryDirectory() as td:
            load = 0x80010000
            payload = b"\0" * 8
            crc = compile_overlays.binascii.crc32(b"\0" * 4) & 0xFFFFFFFF
            logical = Path(td) / "00010000_DEADBEEF.dll"
            entries = set()
            for artifact, entry in (("11111111", load),
                                    ("22222222", load + 4)):
                dll = Path(td) / f"00010000_DEADBEEF_{artifact}.dll"
                dll.write_bytes(b"dll")
                dll.with_suffix(".ranges").write_text(
                    "# psxrecomp overlay code-range manifest v2\n"
                    f"F {entry:08X} {crc:08X}\nR {entry:08X} 4\n",
                    encoding="ascii")

            bundles = list(compile_overlays.validated_cached_bundle_ids(
                str(logical), payload, load, len(payload)))
            self.assertEqual(len(bundles), 2)
            for _dll, _ranges, func_ids, errors in bundles:
                self.assertFalse(errors)
                entries.update(ev for ev, _crc, _func_ranges in func_ids)
            self.assertEqual(entries, {load, load + 4})

    def test_legacy_pair_is_seed_evidence_not_authoritative_coverage(self):
        with tempfile.TemporaryDirectory() as td:
            load = 0x80010000
            payload = b"\0" * 4
            crc = compile_overlays.binascii.crc32(payload) & 0xFFFFFFFF
            logical = Path(td) / "00010000_DEADBEEF.dll"
            logical.write_bytes(b"old-unbound-dll")
            logical.with_suffix(".ranges").write_text(
                "# psxrecomp overlay code-range manifest v2\n"
                f"F {load:08X} {crc:08X}\nR {load:08X} 4\n",
                encoding="ascii")

            seed_evidence = list(
                compile_overlays.validated_cached_bundle_ids(
                    str(logical), payload, load, len(payload)))
            authoritative = list(
                compile_overlays.authoritative_cached_bundle_ids(
                    str(logical), payload, load, len(payload)))
            self.assertEqual(len(seed_evidence), 1)
            self.assertTrue(seed_evidence[0][2])
            self.assertEqual(authoritative, [])

    def test_delay_slot_identity_audit_requires_cross_page_slot(self):
        load = 0x80010FF0
        # NOPs through FF8, BEQ at FFC, ADDIU delay slot at 1000.
        payload = (b"\0" * 12 +
                   (0x10800008).to_bytes(4, "little") +
                   (0x24020001).to_bytes(4, "little"))
        safe = [(load, 0, [(load, len(payload))])]
        unsafe = [(load, 0, [(load, len(payload) - 4)])]
        self.assertEqual(
            compile_overlays.audit_func_id_delay_slots(safe, payload, load), [])
        errors = compile_overlays.audit_func_id_delay_slots(
            unsafe, payload[:-4], load)
        self.assertEqual(len(errors), 1)
        self.assertEqual(errors[0][1], 0x80010FFC)
        self.assertIn("0x80011000", errors[0][2])

    def test_delay_slot_identity_audit_rejects_nested_control_flow(self):
        load = 0x80020000
        # BEQ followed by JR in its delay slot; both words are present, but the
        # interpreter treats control flow in a delay slot as unsupported.
        payload = ((0x10000001).to_bytes(4, "little") +
                   (0x03E00008).to_bytes(4, "little") +
                   (0x00000000).to_bytes(4, "little"))
        ids = [(load, 0, [(load, len(payload))])]
        errors = compile_overlays.audit_func_id_delay_slots(ids, payload, load)
        self.assertTrue(any("control transfer in a delay slot" in e[2]
                            for e in errors))

    def test_delay_slot_identity_audit_rejects_reserved_branch_likely(self):
        load = 0x80020000
        payload = ((0x50800001).to_bytes(4, "little") +
                   (0x00000000).to_bytes(4, "little"))
        ids = [(load, 0, [(load, len(payload))])]
        errors = compile_overlays.audit_func_id_delay_slots(ids, payload, load)
        self.assertTrue(any("reserved/unsupported" in e[2] for e in errors))

    def test_published_manifest_parse_is_all_or_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            load = 0x80010000
            payload = b"\0" * 8
            crc = compile_overlays.binascii.crc32(payload) & 0xFFFFFFFF
            manifest = Path(td) / "bundle.ranges"
            manifest.write_text(
                f"F {load:08X} {crc:08X}\nR {load:08X} 8\n",
                encoding="ascii")
            parsed = compile_overlays.parse_overlay_func_ids(
                str(manifest), payload, load, len(payload),
                require_stored_crc=True)
            self.assertEqual(len(parsed), 1)

            manifest.write_text(
                f"F {load + 0x20000000:08X} {crc:08X}\n"
                f"R {load + 0x20000000:08X} 8\n",
                encoding="ascii")
            self.assertEqual(compile_overlays.parse_overlay_func_ids(
                str(manifest), payload, load, len(payload),
                require_stored_crc=True), [])

            manifest.write_text(
                f"F {load:08X}\nR {load:08X} 8\n", encoding="ascii")
            self.assertEqual(compile_overlays.parse_overlay_func_ids(
                str(manifest), payload, load, len(payload),
                require_stored_crc=True), [])

            manifest.write_text(
                f"F {load:08X} {crc:08X}\nR {load:08X} 8\n"
                f"F {load + 4:08X} 00000000\nR {load + 8:08X} 4\n",
                encoding="ascii")
            self.assertEqual(compile_overlays.parse_overlay_func_ids(
                str(manifest), payload, load, len(payload),
                require_stored_crc=True), [])

    def test_live_dll_publication_is_immutable_and_dll_commits_last(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / "00010000_DEADBEEF.dll"
            source = Path(td) / "source.c"
            source.write_text("/* test */", encoding="utf-8")
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
            observed = []

            def fake_compile(_source, private_dll, _includes, **_kwargs):
                private = Path(private_dll)
                self.assertNotEqual(private, out_dll)
                self.assertFalse(out_dll.exists())
                private.write_bytes(b"complete-dll")
                observed.append(private)
                return True

            real_link = compile_overlays.os.link

            def observe_link(source_path, dest_path):
                observed.append(Path(dest_path))
                real_link(source_path, dest_path)

            with mock.patch.object(compile_overlays, "compile_dll",
                                   side_effect=fake_compile), \
                 mock.patch.object(compile_overlays.os, "link",
                                   side_effect=observe_link):
                ok, count = compile_overlays.compile_and_publish_dll(
                    str(source), str(out_dll), [], func_ids)

            self.assertTrue(ok)
            self.assertEqual(count, 1)
            published = list(Path(td).glob("00010000_*.dll"))
            self.assertEqual(len(published), 1)
            self.assertNotEqual(published[0], out_dll)
            self.assertRegex(
                published[0].name,
                r"^00010000_DEADBEEF_[0-9A-F]{8}\.dll$")
            self.assertEqual(published[0].read_bytes(), b"complete-dll")
            self.assertTrue(published[0].with_suffix(".ranges").exists())
            self.assertEqual(observed[-2:],
                             [published[0].with_suffix(".ranges"), published[0]])
            self.assertFalse(list(Path(td).glob("*.tmp.*")))

    def test_live_publication_first_link_failure_leaves_no_pair(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / "00010000_DEADBEEF.dll"
            source = Path(td) / "source.c"
            source.write_text("/* test */", encoding="utf-8")
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]

            def fake_compile(_source, private_dll, _includes, **_kwargs):
                Path(private_dll).write_bytes(b"complete-dll")
                return True

            with mock.patch.object(compile_overlays, "compile_dll",
                                   side_effect=fake_compile), \
                 mock.patch.object(compile_overlays.os, "link",
                                   side_effect=OSError("simulated DLL lock")):
                ok, count = compile_overlays.compile_and_publish_dll(
                    str(source), str(out_dll), [], func_ids)

            self.assertFalse(ok)
            self.assertEqual(count, 0)
            self.assertFalse(list(Path(td).glob("00010000_*.dll")))
            self.assertFalse(list(Path(td).glob("00010000_*.ranges")))

    def test_live_publication_second_link_failure_leaves_only_ignored_ranges(self):
        with tempfile.TemporaryDirectory() as td:
            out_dll = Path(td) / "00010000_DEADBEEF.dll"
            source = Path(td) / "source.c"
            source.write_text("/* test */", encoding="utf-8")
            func_ids = [(0x80010000, 0x12345678, [(0x80010000, 4)])]
            real_link = compile_overlays.os.link
            calls = 0

            def fake_compile(_source, private_dll, _includes, **_kwargs):
                Path(private_dll).write_bytes(b"complete-dll")
                return True

            def fail_second_link(source_path, dest_path):
                nonlocal calls
                calls += 1
                if calls == 2:
                    raise OSError("simulated ranges lock")
                real_link(source_path, dest_path)

            with mock.patch.object(compile_overlays, "compile_dll",
                                   side_effect=fake_compile), \
                 mock.patch.object(compile_overlays.os, "link",
                                   side_effect=fail_second_link):
                ok, count = compile_overlays.compile_and_publish_dll(
                    str(source), str(out_dll), [], func_ids)

            self.assertFalse(ok)
            self.assertEqual(count, 0)
            self.assertFalse(list(Path(td).glob("00010000_*.dll")))
            self.assertEqual(len(list(Path(td).glob("00010000_*.ranges"))), 1)
            self.assertEqual(compile_overlays.load_region_coverage(
                td, 0x00010000, b"\0" * 4, 0x80010000, 4), set())

    def test_coverage_and_entries_ignore_orphan_ranges(self):
        with tempfile.TemporaryDirectory() as td:
            load = 0x80010000
            payload = b"\0" * 4
            crc = compile_overlays.binascii.crc32(payload) & 0xFFFFFFFF
            ranges = Path(td) / "00010000_DEADBEEF_13579BDF.ranges"
            ranges.write_text(
                "# psxrecomp overlay code-range manifest v2\n"
                f"F 80010000 {crc:08X}\nR 80010000 4\n",
                encoding="utf-8")

            self.assertEqual(compile_overlays.load_region_coverage(
                td, 0x00010000, payload, load, len(payload)), set())
            self.assertEqual(compile_overlays.load_region_entry_set(
                td, 0x00010000, payload, load, len(payload)), set())

            # Legacy DLL/ranges pairs predate bound publication and may be a
            # strict new manifest beside an old DLL after a crash. They remain
            # seed evidence, but cannot suppress the first immutable rebuild.
            legacy = Path(td) / "00010000_DEADBEEF.ranges"
            legacy.write_text(
                "# psxrecomp overlay code-range manifest v2\n"
                f"F 80010000 {crc:08X}\nR 80010000 4\n",
                encoding="utf-8")
            legacy.with_suffix(compile_overlays.overlay_ext()).write_bytes(b"old-dll")
            self.assertFalse(compile_overlays.bundle_path_is_immutable(
                str(legacy.with_suffix(compile_overlays.overlay_ext()))))
            self.assertEqual(compile_overlays.load_region_coverage(
                td, 0x00010000, payload, load, len(payload)), set())
            self.assertEqual(compile_overlays.load_region_entry_set(
                td, 0x00010000, payload, load, len(payload)), set())
            ranges.with_suffix(compile_overlays.overlay_ext()).write_bytes(b"dll")
            self.assertEqual(
                compile_overlays.load_region_coverage(
                    td, 0x00010000, payload, load, len(payload)),
                {(0x80010000, crc)})
            self.assertEqual(
                compile_overlays.load_region_entry_set(
                    td, 0x00010000, payload, load, len(payload)),
                {0x00010000})

            # A paired manifest with a valid-looking prefix and malformed tail
            # must contribute nothing. Otherwise it can poison additive
            # coverage and suppress publication of its own valid repair.
            ranges.write_text(
                "# psxrecomp overlay code-range manifest v2\n"
                f"F 80010000 {crc:08X}\nR 80010000 4\n"
                "TRAILING GARBAGE\n",
                encoding="utf-8")
            self.assertEqual(compile_overlays.load_region_coverage(
                td, 0x00010000, payload, load, len(payload)), set())
            self.assertEqual(compile_overlays.load_region_entry_set(
                td, 0x00010000, payload, load, len(payload)), set())

    def test_unions_evidence_but_preserves_reused_address_variants(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td) / "overlay_captures.json"
            history = Path(str(base) + ".d")
            history.mkdir()
            bytes_a = b"\x01\x02\x03\x04"
            bytes_b = b"\x05\x06\x07\x08"
            (history / "old.json").write_text(json.dumps([
                region(0x80010000, bytes_a, executed=(0x80010000,)),
                region(0x80010000, bytes_b, executed=(0x80010000,)),
            ]), encoding="utf-8")
            base.write_text(json.dumps([
                region(0x80010000, bytes_a, executed=(0x80010004,),
                       dispatch=(0x80010000,)),
            ]), encoding="utf-8")

            captures, sources = compile_overlays.load_additive_captures(str(base))

            self.assertEqual(len(sources), 2)
            self.assertEqual(len(captures), 2)
            by_bytes = {base64.b64decode(c["bytes_b64"]): c for c in captures}
            self.assertEqual(by_bytes[bytes_a]["executed_pcs"],
                             [0x80010000, 0x80010004])
            self.assertEqual(by_bytes[bytes_a]["dispatch_entry_pcs"],
                             [0x80010000])
            self.assertEqual(by_bytes[bytes_b]["executed_pcs"],
                             [0x80010000])

            (history / "truncated.json").write_text("[{", encoding="utf-8")
            recovered, _ = compile_overlays.load_additive_captures(str(base))
            self.assertEqual(len(recovered), 2)
            vault_view = coverage_vault._load_list(str(base))
            self.assertEqual(len(vault_view), 2)

    def test_vault_accepts_history_only_and_unions_all_evidence_fields(self):
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "source.json"
            source_history = Path(str(source) + ".d")
            source_history.mkdir()
            payload = b"\x11\x22\x33\x44"
            (source_history / "only.json").write_text(json.dumps([
                region(0x80020000, payload, executed=(0x80020000,),
                       dispatch=(0x80020000,), functions=(0x80020004,),
                       seeds=(0x80020008,)),
            ]), encoding="utf-8")
            vault = Path(td) / "vault" / "overlay_captures.json"
            vault.parent.mkdir()
            vault.write_text(json.dumps([
                region(0x80020000, payload, executed=(0x8002000C,),
                       functions=(0x80020010,), seeds=(0x80020014,)),
            ]), encoding="utf-8")

            new_variants, new_pcs = coverage_vault.merge_captures(
                str(vault), str(source))
            self.assertEqual((new_variants, new_pcs), (0, 1))
            merged = json.loads(vault.read_text(encoding="utf-8"))[0]
            self.assertEqual(set(merged["executed_pcs"]),
                             {"0x80020000", "0x8002000C"})
            self.assertEqual(set(merged["function_entry_pcs"]),
                             {"0x80020004", "0x80020010"})
            self.assertEqual(set(merged["seeds"]),
                             {"0x80020008", "0x80020014"})

    def test_malformed_record_does_not_discard_valid_sibling(self):
        with tempfile.TemporaryDirectory() as td:
            base = Path(td) / "overlay_captures.json"
            payload = b"\x99\x88\x77\x66"
            invalid_b64 = region(0x80031000, payload)
            invalid_b64["bytes_b64"] = "!!!"
            scalar_evidence = region(0x80032000, payload)
            scalar_evidence["executed_pcs"] = 42
            base.write_text(json.dumps([
                {"load_addr": "not-an-address"},
                invalid_b64,
                scalar_evidence,
                region(0x80030000, payload, executed=(0x80030000,)),
            ]), encoding="utf-8")
            captures, _ = compile_overlays.load_additive_captures(str(base))
            self.assertEqual(len(captures), 1)
            self.assertEqual(captures[0]["load_addr"], "0x80030000")



if __name__ == "__main__":
    unittest.main()
