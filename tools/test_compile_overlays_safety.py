import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays


class OverlaySafetyTests(unittest.TestCase):
    def test_native_continuation_requires_exact_captured_prologue(self):
        load = 0x80780000
        data = bytearray(0x2004)
        offset = compile_overlays.NATIVE_CONTINUATION_PC - load
        data[offset:offset + 4] = \
            compile_overlays.NATIVE_CONTINUATION_WORD.to_bytes(4, 'little')
        self.assertTrue(compile_overlays._has_native_continuation_evidence(
            bytes(data), load, len(data)))
        data[offset] ^= 1
        self.assertFalse(compile_overlays._has_native_continuation_evidence(
            bytes(data), load, len(data)))
        self.assertFalse(compile_overlays._has_native_continuation_evidence(
            bytes(4), load, 4))

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


if __name__ == "__main__":
    unittest.main()
