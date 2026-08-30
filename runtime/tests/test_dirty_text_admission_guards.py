#!/usr/bin/env python3
"""Keep CPU text writes separate from explicit overlay admission."""

from pathlib import Path
import argparse
import sys


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    root = parser.parse_args().root.resolve()
    memory = (root / "runtime/src/memory.c").read_text(encoding="utf-8")
    dma = (root / "runtime/src/dma.c").read_text(encoding="utf-8")

    text_write = function_body(memory, "static inline void text_guard_note_write(")
    if "text_modified_bitmap" not in text_write:
        raise AssertionError("CPU text divergence is no longer recorded")
    for admission in ("dirty_ram_mark_page(", "dirty_ram_mark_executable_range("):
        if admission in text_write:
            raise AssertionError(
                "an ordinary CPU write inside game text automatically admits "
                f"executable overlay code via {admission}"
            )

    cd_slice = function_body(dma, "void dma_advance(")
    read = cd_slice.index("uint32_t word = cdrom_dma_read();")
    store = cd_slice.index("psx_write_word(addr, word);", read)
    admit = cd_slice.index("dirty_ram_mark_executable_range(addr, 4);", store)
    advance = cd_slice.index("addr = (addr + addr_step)", admit)
    if not read < store < admit < advance:
        raise AssertionError("CD DMA overlay admission is not coupled to each RAM word")

    mark_range = function_body(memory, "void dirty_ram_mark_executable_range(")
    for fragment in (
        "dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));",
        "g_dirty_ram_code_gen++;",
    ):
        if fragment not in mark_range:
            raise AssertionError(f"explicit executable-range admission lost: {fragment}")

    print("dirty-text admission guards: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
