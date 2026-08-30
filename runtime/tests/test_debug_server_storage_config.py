#!/usr/bin/env python3
"""Keep disabled debug-server storage out of lean product builds."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

RUNTIME = Path(__file__).resolve().parents[1]
SOURCE = (RUNTIME / "src" / "debug_server.c").read_text(encoding="utf-8")
HEADER = (RUNTIME / "include" / "debug_server.h").read_text(encoding="utf-8")

INLINE_PRODUCERS = {
    "debug_server_log_restore_event": ("kind", "target_pc", "jmp_val"),
    "debug_server_log_thread_event": (
        "kind", "cpu", "current_tcb", "target_tcb", "target_pc",
    ),
    "debug_server_log_sio_write": ("addr", "value", "width"),
}

DIAGNOSTIC_CAPS = {
    "FP_RING_CAP",
    "REC_CAP",
    "WRITE_TRACE_CAP",
    "WRITE_TRACE_BOOT_CAP",
    "WRITE_TRACE_ALL_CAP",
    "WRITE_TRACE_TRANS_CAP",
    "SIO_PC_TRACE_CAP",
    "SIO_CTRL_REG_TRACE_CAP",
    "RESTORE_TRACE_CAP",
    "THREAD_TRACE_CAP",
    "SREG_TRACE_CAP",
    "PROBE_TRACE_CAP",
    "DISPATCH_TRACE_CAP",
    "UNKNOWN_DISPATCH_CAP",
    "UNKNOWN_UNIQUE_CAP",
    "DISPATCH_UNIQUE_CAP",
    "CHAIN_TRACE_CAP",
    "FN_TRACE_CAP",
    "FN_EXIT_TRACE_CAP",
    "CALL_FOCUS_CAP",
    "CARD_MGR_TRACE_CAP",
    "EVCB_RING_CAP",
    "CE_CAP",
    "BIOSCALL_RING_CAP",
    "BIOSCALL_UNIQUE_CAP",
    "CYC_WATCH_RING_CAP",
    "PC_PROBE_SAMPLE_CAP",
    "MMIO_TRACE_CAP",
    "GP1_TRACE_CAP",
    "DISP_RING_CAP",
    "CARD_TRACE_CAP",
}


class DebugServerStorageConfigTests(unittest.TestCase):
    def test_lean_capacity_macro_keeps_one_legal_slot(self):
        self.assertRegex(
            SOURCE,
            r"#ifdef PSX_NO_DEBUG_TOOLS\s+"
            r"#\s*define PSX_DEBUG_RING_CAP\(debug_cap_\) 1u\s+"
            r"#else\s+"
            r"#\s*define PSX_DEBUG_RING_CAP\(debug_cap_\) \(debug_cap_\)",
        )

    def test_every_diagnostic_capacity_uses_lean_gate(self):
        missing = sorted(
            cap for cap in DIAGNOSTIC_CAPS
            if not re.search(
                rf"^#define\s+{cap}\s+PSX_DEBUG_RING_CAP\(",
                SOURCE,
                flags=re.MULTILINE,
            )
        )
        self.assertEqual(missing, [])

    def test_hot_producers_inline_away_only_in_lean_builds(self):
        lean_header = HEADER.split("#ifdef PSX_NO_DEBUG_TOOLS", 1)[1]
        for name, parameters in INLINE_PRODUCERS.items():
            match = re.search(
                rf"static inline void {name}\([^}}]+\)\s*\{{([^}}]+)\}}",
                lean_header,
            )
            self.assertIsNotNone(match, f"missing lean inline for {name}")
            body = match.group(1)
            for parameter in parameters:
                self.assertIn(f"(void){parameter};", body)
            self.assertRegex(
                SOURCE,
                rf"#ifndef PSX_NO_DEBUG_TOOLS\s+void {name}\(",
            )

    def test_lean_callers_have_no_external_producer_references(self):
        compiler = next(
            (path for name in ("gcc", "clang", "cc")
             if (path := shutil.which(name))),
            None,
        )
        if compiler is None:
            self.skipTest("gcc/clang is not available for the inline-call check")

        harness = '''#include "debug_server.h"
void exercise_debug_producers(CPUState *cpu) {
    debug_server_log_restore_event(1u, 2u, 3u);
    debug_server_log_thread_event(1u, cpu, 2u, 3u, 4u);
    debug_server_log_sio_write(1u, 2u, 4u);
}
'''
        with tempfile.TemporaryDirectory(prefix="debug-inline-") as temp_name:
            temp = Path(temp_name)
            source = temp / "producer_calls.c"
            source.write_text(harness, encoding="utf-8")
            outputs = {}
            for mode, definitions in (
                ("lean", ["-DPSX_NO_DEBUG_TOOLS=1"]),
                ("debug", []),
            ):
                assembly = temp / f"producer_calls_{mode}.s"
                subprocess.run(
                    [compiler, "-std=c11", "-O2", *definitions,
                     f"-I{RUNTIME / 'include'}", "-S", str(source),
                     "-o", str(assembly)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                outputs[mode] = assembly.read_text(
                    encoding="utf-8", errors="replace")
            for name in INLINE_PRODUCERS:
                self.assertNotIn(name, outputs["lean"])
                self.assertIn(name, outputs["debug"])

if __name__ == "__main__":
    unittest.main()
