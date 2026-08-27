#!/usr/bin/env python3
"""Structural exactness guards for the round-3 hot-path specializations."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    if not match:
        raise AssertionError(f"missing function {name}")
    depth = 1
    for pos in range(match.end(), len(source)):
        depth += source[pos] == "{"
        depth -= source[pos] == "}"
        if depth == 0:
            return source[match.end():pos]
    raise AssertionError(f"unterminated function {name}")


def check_irq_writers() -> None:
    files = [
        "runtime/src/boot_state.c",
        "runtime/src/interrupts.c",
        "runtime/src/memory.c",
        "runtime/src/overlay_loader.c",
        "runtime/src/sio.c",
    ]
    mutation = re.compile(
        r"(?<![>.])\bi_(?:stat|mask)\s*(?:=(?!=)|\|=|&=|\^=)")
    seen = 0
    for rel in files:
        source = (ROOT / rel).read_text(encoding="utf-8")
        for match in mutation.finditer(source):
            seen += 1
            tail = source[match.end():match.end() + 650]
            if "psx_irq_refresh_cause_ip2();" not in tail:
                line = source.count("\n", 0, match.start()) + 1
                raise AssertionError(f"stale IRQ pending-cache writer: {rel}:{line}")
    if seen != 19:
        raise AssertionError(f"IRQ writer set changed (expected 19, found {seen}); audit it")


def check_highram_static_generation_invalidation(memory: str, loader: str) -> None:
    """Keep static high-RAM memo verdicts tied to the live DMA image.

    The generated high-RAM dispatcher bypasses overlay_loader registration, so
    this is intentionally a source-level contract check over the same reset,
    write, and memo paths used by the runtime. It catches the regression where
    every page generation remained zero and a cached matching variant survived
    a later image replacement.
    """
    reset = function_body(memory, "dirty_ram_reset_for_boot")
    arm = reset.find("overlay_watch_set_range(0x00780000u, 0x2004u);")
    clear = reset.find("memset(overlay_watch_bitmap, 0, sizeof(overlay_watch_bitmap));")
    if arm < 0:
        raise AssertionError("pinned high-RAM static image is not generation-watched at boot")
    if clear < 0 or arm < clear:
        raise AssertionError("high-RAM watch arm must follow the boot generation reset")
    if "0x00800000u" in reset[arm - 180:arm + 120]:
        raise AssertionError("high-RAM invalidation watch widened beyond the pinned image")

    note = function_body(memory, "overlay_watch_note_write")
    watched = note.find("if ((overlay_watch_bitmap[pg >> 5] >> (pg & 31u)) & 1u)")
    bump = note.find("overlay_page_gen[pg]++;")
    if watched < 0 or bump < 0 or watched > bump:
        raise AssertionError("writes in the pinned high-RAM image do not bump its page generation")

    memo = function_body(loader, "psx_overlay_static_code_matches_memo")
    pagegen = memo.find("overlay_watch_pagegen_sum(lo, len);")
    fast = memo.find("if ((state[1] & 1u) && state[0] == gen_sum)")
    crc = memo.find("uint32_t crc = 0xFFFFFFFFu;")
    save = memo.find("state[0] = gen_sum;")
    if min(pagegen, fast, crc, save) < 0 or not pagegen < fast < crc < save:
        raise AssertionError("static memo can bypass rehash after a watched-page generation change")


def main() -> None:
    memory = (ROOT / "runtime/src/memory.c").read_text(encoding="utf-8")
    loader = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")
    cycles = (ROOT / "runtime/src/psx_cycles.c").read_text(encoding="utf-8")
    cyc_steps = (ROOT / "runtime/src/psx_cyc_steps.c").read_text(encoding="utf-8")
    fntrace = (ROOT / "runtime/src/fntrace.c").read_text(encoding="utf-8")
    text = (ROOT / "runtime/src/text_xlate.cpp").read_text(encoding="utf-8")
    codegen = (ROOT / "recompiler/src/code_generator.cpp").read_text(encoding="utf-8")
    fullgen = (ROOT / "recompiler/src/full_function_emitter.cpp").read_text(encoding="utf-8")

    check_irq_writers()
    check_highram_static_generation_invalidation(memory, loader)

    refresh = function_body(
        (ROOT / "runtime/src/interrupts.c").read_text(encoding="utf-8"),
        "psx_irq_refresh_cause_ip2")
    pending_store = refresh.find("g_psx_irq_hw_pending = pending;")
    null_return = refresh.find("if (!s_cause_ptr) return;")
    if min(pending_store, null_return) < 0 or pending_store > null_return:
        raise AssertionError("IRQ pending cache depends on Cause pointer installation")

    # Release builds have no consumer for card-data diagnostics. Every universal
    # RAM-store call and the SIO arming site must preprocess away with debug tools.
    for call in re.finditer(r"card_data_writes_(?:check|arm)\s*\(",
                            memory + (ROOT / "runtime/src/sio.c").read_text(encoding="utf-8")):
        prefix = (memory + (ROOT / "runtime/src/sio.c").read_text(encoding="utf-8"))[
            max(0, call.start() - 500):call.start()]
        if prefix.rfind("#ifndef PSX_NO_DEBUG_TOOLS") < prefix.rfind("#endif"):
            raise AssertionError("card-data diagnostic call escapes its release-build guard")

    memo = function_body(memory, "dirty_ram_text_native_ok_ranges_from")
    for exact_guard in (
        "if (memo->epoch == epoch)",
        "memo->gen == text_guard_ranges_gen(lo_len_pairs, count)",
        "memo->epoch = epoch",
        "memo->epoch   = g_text_guard_epoch",
    ):
        if exact_guard not in memo:
            raise AssertionError(f"text memo lost exact two-level guard: {exact_guard}")
    note_write = function_body(memory, "text_guard_note_write")
    if "g_text_guard_epoch++;" not in note_write:
        raise AssertionError("a diverging guarded write does not invalidate the epoch")

    for observer in ("psx_muldiv_set", "psx_muldiv_stall", "psx_gte_read",
                     "psx_gte_set", "psx_gte_stall"):
        body = function_body(cycles, observer)
        if body.find("psx_cyc_batch_flush();") < 0 or body.find(
                "psx_cyc_batch_flush();") > body.find("psx_cycle_count"):
            raise AssertionError(f"{observer} reads a deferred cycle counter")
    if "#if !defined(PSX_COSIM)" not in function_body(
            cyc_steps, "psx_cyc_generated_base"):
        raise AssertionError("specialized cycle base bypasses COSIM accounting")
    for emitter in (codegen, fullgen):
        helper = function_body(emitter, "emit_cyc_step")
        for arity in range(4):
            if f"psx_cyc_step_{arity}" not in helper:
                raise AssertionError(f"emitter lost cycle-step arity {arity}")
        if "psx_cyc_step(cpu" not in helper:
            raise AssertionError("emitter lost generic dependency-mask fallback")

    if "if (text_xlate_dispatch_armed())" not in fntrace:
        raise AssertionError("dispatch translation hook is not aggregate-gated")
    armed = function_body(text, "refresh_dispatch_armed")
    for source in ("g_capture_on", "g_string_table_nonempty", "g_glyph_pending",
                   "g_msg_inplace_pending", "g_msg_sep_pending"):
        if source not in armed:
            raise AssertionError(f"translation dispatch gate omits {source}")

    print("test_perf_round3_guards: all checks passed")


if __name__ == "__main__":
    main()
