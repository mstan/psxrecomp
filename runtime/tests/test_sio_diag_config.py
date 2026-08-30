#!/usr/bin/env python3
"""Verify SIO diagnostic rings exist only in debug-tool builds."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from test_cd_spu_trace_config import (
    compile_and_preprocess,
    is_msvc,
    object_info,
    require,
    simple_function_body,
)

MIB = 1024 * 1024
TXN_BYTES = 544 * (1 << 16)
IRQ_BYTES = 40 * (1 << 20)
RING_BYTES = TXN_BYTES + IRQ_BYTES


def compile_harness(compiler: Path, runtime: Path, lean: bool,
                    temp: Path) -> Path:
    source = temp / ("sio-getters-lean.c" if lean else "sio-getters-debug.c")
    executable = temp / ("sio-getters-lean.exe" if lean else
                         "sio-getters-debug.exe")
    source.write_text(r'''#include "sio.h"
#include <stdint.h>
#include <stdio.h>

int main(void) {
    const SioTxnEntry *txns = (const SioTxnEntry *)(uintptr_t)1;
    const SioTxnEntry *live;
    const SioIrqEntry *irqs = (const SioIrqEntry *)(uintptr_t)1;
    int txn_idx = -1, open = -1, irq_idx = -1;
    uint32_t txn_total = sio_get_card_txns(&txns, &txn_idx, &open);
    uint32_t irq_total = sio_get_irq_ring(&irqs, &irq_idx);
    live = sio_get_card_txn_live();
#ifdef PSX_NO_DEBUG_TOOLS
    if (txns || irqs || live || txn_idx || open || irq_idx ||
        txn_total || irq_total) return 1;
#else
    if (!txns || !irqs || live || txn_idx || open || irq_idx ||
        txn_total || irq_total) return 1;
#endif
    printf("%zu %zu\n", sizeof(SioTxnEntry), sizeof(SioIrqEntry));
    return 0;
}
''', encoding="utf-8")

    sio = runtime / "src/sio.c"
    behavior = runtime / "tests/test_sio_dualshock_rumble.c"
    include = runtime / "include"
    define = (["/DPSX_NO_DEBUG_TOOLS=1"] if is_msvc(compiler) else
              ["-DPSX_NO_DEBUG_TOOLS=1"]) if lean else []
    if is_msvc(compiler):
        # MSVC's linker cannot garbage-collect the many unrelated sio.c
        # dependencies as portably as the GNU/LLVM driver used by this test.
        raise AssertionError("SIO getter behavior harness requires clang or gcc")
    common = [str(compiler), "-std=gnu99", "-O0", *define,
              f"-I{include}", f"-I{sio.parent}"]
    objects = []
    for stem, input_source, extra in (
        ("harness", source, []),
        ("behavior", behavior, ["-Dmain=sio_dualshock_main"]),
        ("sio", sio, []),
    ):
        obj = temp / f"{stem}-{'lean' if lean else 'debug'}.o"
        subprocess.run([*common, *extra, "-c", str(input_source), "-o", str(obj)],
                       check=True)
        objects.append(obj)
    subprocess.run([str(compiler), *map(str, objects), "-o", str(executable)],
                   check=True)
    return executable


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    source = runtime / "src/sio.c"
    include = runtime / "include"

    with tempfile.TemporaryDirectory(prefix="sio-diag-") as temp_name:
        temp = Path(temp_name)
        suffix = ".obj" if is_msvc(args.compiler) else ".o"
        debug_obj = temp / ("sio-debug" + suffix)
        lean_obj = temp / ("sio-lean" + suffix)
        debug_pp = compile_and_preprocess(args.compiler, source, include,
                                          False, debug_obj)
        lean_pp = compile_and_preprocess(args.compiler, source, include,
                                         True, lean_obj)

        for declaration in (
            "SioTxnEntry sio_txn_buf[(1 << 16)]",
            "SioIrqEntry sio_irq_buf[(1 << 20)]",
        ):
            require(declaration in debug_pp,
                    f"debug storage missing after preprocessing: {declaration}")
            require(declaration not in lean_pp,
                    f"lean storage survived preprocessing: {declaration}")
        for recording in (
            "sio_txn_buf[sio_txn_idx] = sio_txn_cur",
            "SioIrqEntry *e = &sio_irq_buf[sio_irq_idx]",
        ):
            require(recording in debug_pp,
                    f"debug recording path missing: {recording}")
            require(recording not in lean_pp,
                    f"lean recording path survived: {recording}")

        for name in ("sio_get_card_txns", "sio_get_card_txn_live",
                     "sio_get_irq_ring"):
            require(name in object_info(lean_obj)[0],
                    f"lean object lost public getter symbol {name}")
        forbidden = {"sio_txn_buf", "sio_txn_idx", "sio_txn_seq",
                     "sio_irq_buf", "sio_irq_idx"}
        debug_symbols, debug_bss = object_info(debug_obj)
        lean_symbols, lean_bss = object_info(lean_obj)
        require(forbidden <= debug_symbols,
                f"debug object lost ring symbols: {sorted(forbidden - debug_symbols)}")
        require(not (forbidden & lean_symbols),
                f"lean object retained ring symbols: {sorted(forbidden & lean_symbols)}")
        require(debug_bss - lean_bss >= RING_BYTES,
                f"lean object saved only {debug_bss - lean_bss} B of BSS")

        card_body = simple_function_body(lean_pp, "sio_get_card_txns")
        irq_body = simple_function_body(lean_pp, "sio_get_irq_ring")
        require("*buf_out" in card_body and "return 0;" in card_body,
                "lean card getter is not a NULL/zero stub")
        require("*buf_out" in irq_body and "return 0;" in irq_body,
                "lean IRQ getter is not a NULL/zero stub")
        live_body = simple_function_body(lean_pp, "sio_get_card_txn_live")
        require(live_body.startswith("return ") and live_body.endswith("0);"),
                "lean live getter is not a NULL stub")

        if not is_msvc(args.compiler):
            outputs = []
            for lean in (False, True):
                executable = compile_harness(args.compiler, runtime, lean, temp)
                result = subprocess.run([str(executable)], check=True,
                                        stdout=subprocess.PIPE, text=True)
                outputs.append(result.stdout.strip())
            require(outputs == ["544 40", "544 40"],
                    f"unexpected getter/layout/behavior output: {outputs}")

    print("PASS: SIO lean diagnostic contract "
          f"(TXN -{TXN_BYTES} B, IRQ -{IRQ_BYTES} B, total -{RING_BYTES} B "
          f"= {RING_BYTES / MIB:.2f} MiB)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
