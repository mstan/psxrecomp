#!/usr/bin/env python3
"""Static isolated fragments are demanded per (entry, image), not per address.

The content-validated dispatcher accepts a variant only when the resident
bytes hash to the CRC that variant was compiled from. So a fragment built
from one capture's bytes serves nothing while a sibling capture of the same
band is resident. compile_overlays --static used to key the post-pass on
the bare entry address: the first variant at an address, from any capture,
marked that address served for every capture of the band, and the per-entry
source map kept only the last capture's bytes. In a band with N occupants
exactly one occupant ever received fragments (BoF3 SCENARIO band,
2026-09-05: 20 sections, SCENA16 resident, all 53 of its observed entries
interpreted while every spanning piece had been compiled from SCENA19/04/06).

This pins the keying: two captures that both observed an address are two
demands, and a part serves an address only for its own image.
"""

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import compile_overlays  # noqa: E402


def part(image_crc, *addrs):
    return {
        "image_crc": image_crc,
        "variants": [{"addr": a, "symbol": f"f_{a:08X}", "crc": 1, "ranges": ()}
                     for a in addrs],
    }


class StaticFragmentVariantKeyTests(unittest.TestCase):
    ENTRY = 0x801F6C90
    A, B = 0xDF19D713, 0x1E5EE588      # SCENA16 / SCENA19 image crcs

    def test_a_part_serves_its_own_image_only(self):
        served = compile_overlays.served_static_variant_keys(
            [part(self.B, self.ENTRY)])
        self.assertIn((self.ENTRY, self.B), served)
        self.assertNotIn((self.ENTRY, self.A), served)

    def test_sibling_capture_of_the_same_address_is_still_demanded(self):
        requested = {(self.ENTRY, self.A), (self.ENTRY, self.B)}
        demands = compile_overlays.select_static_fragment_demands(
            requested, [part(self.B, self.ENTRY)])
        # B's own fragment exists; A's demand at the same address survives.
        self.assertEqual(demands, [(self.ENTRY, self.A)])

    def test_region_part_of_the_same_image_serves_the_demand(self):
        requested = {(self.ENTRY, self.A)}
        demands = compile_overlays.select_static_fragment_demands(
            requested, [part(self.A, 0x801F6C00, self.ENTRY)])
        self.assertEqual(demands, [])

    def test_demands_are_sorted_and_distinct(self):
        requested = [(self.ENTRY, self.B), (self.ENTRY, self.A),
                     (self.ENTRY, self.A), (0x801F6C00, self.A)]
        demands = compile_overlays.select_static_fragment_demands(
            requested, [])
        # Sorted by (entry, image crc); B < A numerically.
        self.assertEqual(demands, [(0x801F6C00, self.A),
                                   (self.ENTRY, self.B),
                                   (self.ENTRY, self.A)])

    def test_capture_job_keys_requests_by_entry_and_image(self):
        # The producer side of the contract: a capture's requested entries
        # come back keyed with its own image crc, so main() can never fold two
        # captures' demands at one address into one.
        import base64
        import binascii
        data = bytes(range(64)) * 4                 # 256 bytes, any content
        cap = {"load_addr": "0x801F6C00", "size": len(data),
               "bytes_b64": base64.b64encode(data).decode("ascii"),
               "dispatch_entry_pcs": ["0x801F6C90"], "guard_bytes": 0}
        crc = binascii.crc32(data) & 0xFFFFFFFF
        result = {"label": None, "outcome": None, "fail": None, "part": None,
                  "requested_entries": set(), "entry_sources": {}, "log": ""}

        class Args:
            recompiler = "/nonexistent/recompiler"
            game_toml = str(ROOT / "nonexistent.toml")
            cps = False
            project_root = None
        try:
            compile_overlays._static_capture_job(
                cap, Args(), {}, set(), "/nonexistent/out", result)
        except Exception:  # noqa: BLE001 -- the recompiler is absent; the
            pass           # request bookkeeping runs before it is invoked
        self.assertEqual(result["requested_entries"], {(0x801F6C90, crc)})
        self.assertIn((0x801F6C90, crc), result["entry_sources"])


if __name__ == "__main__":
    unittest.main()
