#!/usr/bin/env python3
"""Regression checks for append-only overlay capture history ingestion."""
import importlib.util
import json
import pathlib
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "coverage_vault", ROOT / "tools" / "coverage_vault.py")
MOD = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOD)


def region(addr, payload, pcs):
    return {
        "schema": "psxrecomp overlay capture v2",
        "load_addr": addr,
        "size": 4,
        "bytes_b64": payload,
        "executed_pcs": pcs,
        "dispatch_entry_pcs": pcs,
        "function_entry_pcs": [],
        "seeds": pcs,
    }


def main():
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        addendum = root / "overlay_captures.addendum.jsonl"
        vault = root / "vault" / "overlay_captures.json"
        first = region("0x80100000", "AAAAAA==", ["0x80100000"])
        second = region("0x80100000", "AQAAAA==", ["0x80100004"])
        records = [
            {"schema": "psxrecomp overlay capture addendum v1",
             "captures": [first]},
            {"schema": "psxrecomp overlay capture addendum v1",
             "captures": [first, second]},
        ]
        with addendum.open("w", encoding="utf-8", newline="\n") as out:
            out.write(json.dumps(records[0], separators=(",", ":")) + "\n")
            # A later launch first quarantines a hard-killed partial record.
            out.write('{"schema":"torn-tail"\n')
            out.write(json.dumps({"schema": "unrelated", "captures": [second]},
                                 separators=(",", ":")) + "\n")
            out.write(json.dumps(records[1], separators=(",", ":")) + "\n")

        new_variants, new_pcs = MOD.merge_addendum(str(vault), str(addendum))
        assert new_variants == 2
        assert new_pcs == 2
        merged = json.loads(vault.read_text(encoding="utf-8"))
        assert len(merged) == 2
        assert not list(vault.parent.glob("*.tmp-*"))

        # Re-ingesting the same durable history is additive and idempotent.
        assert MOD.merge_addendum(str(vault), str(addendum)) == (0, 0)
        print("ALL PASS")


if __name__ == "__main__":
    main()
