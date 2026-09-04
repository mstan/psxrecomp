#!/usr/bin/env python3
"""stall_report.py - one-command interpreter/stall attribution for any title.

Answers, in a single pasteable report:

  1. Is the overlay cache actually BEING USED, or merely present on disk?
     (overlay_loader_status dispatch_native vs dispatch_interp_fallback,
     autocompile degraded state, shard pass/fail counts)
  2. When code IS interpreted, how badly is it thrashing the dispatcher?
     (dirty_ram_stats insns_run / blocks_run - instructions per dispatcher
     round-trip. The kernel window [0,0x10000) intentionally stays per-block,
     so a low ratio there is the known pathology; overlay-region code chains
     locally and runs orders of magnitude more instructions per round-trip.)
  3. WHERE did the wall-clock go, and was the guest even running?
     (starv_ring PC samples: the largest host_us gaps, each with the guest
     cycles that advanced across it. A stall where guest cycles advance
     slowly is CPU-bound execution; a stall where they barely advance at all
     is the emu thread not running - a completely different bug.)

PASSIVE RING CONSUMER - this is the whole point. It does not arm a trace, it
does not pause, and it does not single-step. Every number is either a
cumulative-since-boot counter (two snapshots -> a window by subtraction) or a
read of the bounded starvation ring. The ring overwrites old entries. Inspect
its reported span before you attribute an earlier event.

Usage:

  # One-shot: cumulative state since boot, plus the worst stalls in the ring.
  py -3 tools/stall_report.py --port 4370 snap

  # Windowed: two snapshots N seconds apart. Play through the slow thing
  # during the window. Cumulative counters are deltas. Phase shares describe
  # a bounded rolling window near the second snapshot.
  py -3 tools/stall_report.py --port 4370 run --secs 60 --out report.json

The runtime must be built with PSX_DEBUG_TOOLS=ON - a Release build ships no
TCP debug server and this tool will simply fail to connect.
"""
import argparse
import hashlib
import json
import math
import os
import socket
import sys
import time
from pathlib import Path

X1_MCYC_PER_S = 33.8688      # NTSC PSX CPU clock: guest Mcyc per real second at 1x
KERNEL_WINDOW_END = 0x10000  # DIRTY_RAM_KERNEL_WINDOW_END
PHASE_RING_MAX_WINDOW = 62
PHASE_HOT_TOP = 64
STARVATION_RING_CAP = 1 << 14
REPORT_SCHEMA_VERSION = 1


class ReportError(RuntimeError):
    """The runtime evidence is incomplete or cannot be compared safely."""


class Client:
    """One connection per command.

    The debug server aborts a second request on the same socket, so every
    command reconnects - same contract as tools/debug_client.py and
    tools/load_probe.py.
    """

    def __init__(self, host, port, timeout=8.0):
        self.host, self.port, self.timeout = host, port, timeout
        self.next_id = 1

    def cmd(self, name, **params):
        request_id = self.next_id
        req = {"cmd": name, "id": request_id}
        req.update(params)
        self.next_id += 1
        blob = (json.dumps(req) + "\n").encode()

        def validate_reply(reply):
            if not isinstance(reply, dict):
                return {"ok": False, "error": "reply is not an object"}
            reply_id = reply.get("id")
            ping_fast_path = (name == "ping" and reply_id == 0 and
                              reply.get("pong") is True and
                              reply.get("io_thread") is True)
            if reply_id != request_id and not ping_fast_path:
                return {"ok": False,
                        "error": (f"response id mismatch: expected {request_id}, "
                                  f"got {reply_id}")}
            return reply

        try:
            with socket.create_connection((self.host, self.port),
                                          timeout=self.timeout) as s:
                s.sendall(blob)
                chunks = []
                while True:
                    b = s.recv(65536)
                    if not b:
                        break
                    chunks.append(b)
                    if b.endswith(b"\n") or b"}" in b[-1:]:
                        # The server sends one JSON object then waits; try to
                        # parse what we have and stop as soon as it is whole.
                        try:
                            reply = json.loads(b"".join(chunks).decode().strip())
                            return validate_reply(reply)
                        except json.JSONDecodeError:
                            continue
                raw = b"".join(chunks).decode().strip()
                if not raw:
                    return {"ok": False, "error": "empty reply"}
                reply = json.loads(raw)
                return validate_reply(reply)
        except (OSError, json.JSONDecodeError) as e:
            return {"ok": False, "error": f"{type(e).__name__}: {e}"}


# Commands whose whole payload we keep for the digest and the JSON artifact.
BASE_SNAP_COMMANDS = (
    ("overlay_loader_status", {}),
    ("autocompile_status",    {}),
    ("dirty_ram_stats",       {}),
    ("kernel_bless",          {}),
    ("frame_perf",            {}),
)


def snapshot(c, phase_window=10, hot_top=PHASE_HOT_TOP):
    """Read each required command and fail closed on incomplete evidence."""
    commands = BASE_SNAP_COMMANDS + (
        ("phase_profile", {"window": phase_window}),
        ("phase_hot", {"set": "native", "top": hot_top}),
        ("phase_hot", {"set": "static", "top": hot_top}),
    )
    snap = {"wall": time.time(), "phase_window_requested_s": phase_window,
            "phase_hot_requested_top": hot_top}
    for name, params in commands:
        key = name if not params.get("set") else f'{name}:{params["set"]}'
        reply = c.cmd(name, **params)
        if not isinstance(reply, dict) or not reply.get("ok", False):
            detail = (reply.get("error", "invalid reply")
                      if isinstance(reply, dict) else "invalid reply")
            if name == "frame_perf":
                snap[key] = reply
                snap.setdefault("warnings", []).append(
                    f"optional {key} unavailable: {detail}")
                continue
            raise ReportError(f"{key} failed: {detail}")
        snap[key] = reply
    return snap


def num(d, key, default=0):
    """Read a numeric field that may be absent or a hex string."""
    if not isinstance(d, dict):
        return default
    v = d.get(key, default)
    if isinstance(v, str):
        try:
            return int(v, 16) if v.startswith("0x") else int(v)
        except ValueError:
            return default
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else default


def require_num(d, key, context):
    """Read a required integer field without converting protocol drift to zero."""
    if not isinstance(d, dict) or key not in d:
        raise ReportError(f"missing field: {context}.{key}")
    value = d[key]
    if isinstance(value, bool):
        raise ReportError(f"invalid numeric field: {context}.{key}")
    if isinstance(value, str):
        try:
            return int(value, 16) if value.startswith("0x") else int(value)
        except ValueError as exc:
            raise ReportError(f"invalid numeric field: {context}.{key}") from exc
    if not isinstance(value, int):
        raise ReportError(f"invalid numeric field: {context}.{key}")
    return value


def require_real(d, key, context):
    """Read a required JSON real value, rejecting booleans and missing fields."""
    if not isinstance(d, dict) or key not in d:
        raise ReportError(f"missing field: {context}.{key}")
    value = d[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ReportError(f"invalid real field: {context}.{key}")
    return value


def require_object(d, key, context):
    if not isinstance(d, dict) or not isinstance(d.get(key), dict):
        raise ReportError(f"missing object: {context}.{key}")
    return d[key]


def delta(a, b, cmd, key):
    """b - a, or b's absolute value in snap mode.

    `a is None` means snap mode: there is no earlier snapshot, so the honest
    number is the cumulative one. Diffing a snapshot against itself would
    report zero for every counter and read as "nothing is happening", which is
    the opposite of the truth on a healthy warm-cache run.
    """
    if a is None:
        return require_num(b.get(cmd), key, cmd)
    result = (require_num(b.get(cmd), key, cmd) -
              require_num(a.get(cmd), key, cmd))
    if result < 0:
        raise ReportError(f"counter reset: {cmd}.{key}")
    return result


def object_delta(a, b, key, context):
    if a is None:
        return require_num(b, key, context)
    result = require_num(b, key, context) - require_num(a, key, context)
    if result < 0:
        raise ReportError(f"counter reset: {context}.{key}")
    return result


def phot_map(reply):
    """phase_hot -> {addr: samples}, so two snapshots can be subtracted."""
    out = {}
    if isinstance(reply, dict):
        for index, e in enumerate(reply.get("top", []) or []):
            address = e.get("addr") if isinstance(e, dict) else None
            if not isinstance(address, str) or not address:
                raise ReportError(f"invalid phase_hot.top[{index}].addr")
            out[address] = require_num(e, "samples", f"phase_hot.top[{index}]")
    return out


def phot_delta_lines(a, b, limit=12):
    m0, m1 = ({} if a is None else phot_map(a)), phot_map(b)
    if a is None:
        addresses = set(m1)
    else:
        addresses = set(m0) & set(m1)
    diffs = [(addr, m1[addr] - m0.get(addr, 0)) for addr in addresses]
    if any(d < 0 for _, d in diffs):
        raise ReportError("phase_hot counter reset")
    diffs = [(addr, d) for addr, d in diffs if d > 0]
    diffs.sort(key=lambda kv: -kv[1])
    lines = [f"      {addr}  {d:>12,}" for addr, d in diffs[:limit]]
    if a is not None:
        before_only = len(set(m0) - set(m1))
        after_only = len(set(m1) - set(m0))
        if before_only or after_only:
            lines.append("      NOTE: localization is partial. Top-set censoring "
                         f"removed {before_only} baseline and {after_only} end entries.")
    return lines


def hot_set_warning(reply, requested):
    """Explain that a returned top-N set is censored, not a full histogram."""
    rows = reply.get("top", []) if isinstance(reply, dict) else []
    drops = require_num(reply, "hash_drops", "phase_hot")
    if drops:
        return (f"      NOTE: phase-hot hash table dropped {drops} samples. "
                "Localization is incomplete.")
    if len(rows) >= requested:
        return (f"      NOTE: returned {len(rows)} of at least {requested} hot "
                "entries. Window deltas are censored.")
    return None


def stalls_from_ring(c, count=2048, top=12):
    """Locate the largest wall-clock gaps in the always-on PC-sample ring.

    kind 15 (SR_EVT_PC_SAMPLE) entries carry (cyc, us, func). Consecutive
    deltas give both how much real time passed and how many guest cycles the
    CPU advanced across it, which is what separates the two failure shapes:

      guest advancing near 1x  -> the frame was long but the CPU was working
      guest advancing slowly   -> CPU-bound, executing something expensive
      guest barely advancing   -> the emu thread was not running at all
                                  (host-side block: I/O, driver, scheduler)
    """
    r = c.cmd("starv_ring", count=count, kind=15)
    if not r.get("ok", False):
        return None, r.get("error", "starv_ring failed"), [], {}
    entries = r.get("entries", r.get("ring", [])) or []
    total_events = require_num(r, "total", "starv_ring")
    seqs = [require_num(e, "seq", f"starv_ring.entries[{index}]")
            for index, e in enumerate(entries)]
    meta = {
        "capacity": STARVATION_RING_CAP,
        "requested": count,
        "returned": len(entries),
        "request_full": len(entries) >= count,
        "total_events": total_events,
        "ring_has_wrapped": total_events > STARVATION_RING_CAP,
        "oldest_returned_seq": min(seqs) if seqs else None,
        "newest_returned_seq": max(seqs) if seqs else None,
        "scope": "last returned PC samples only",
        "raw_entries": entries,
    }
    samples = []
    for index, e in enumerate(entries):
        samples.append((require_num(e, "us", f"starv_ring.entries[{index}]"),
                        require_num(e, "cyc", f"starv_ring.entries[{index}]"),
                        e.get("func", "?"),
                        require_num(e, "in_exc", f"starv_ring.entries[{index}]")))
        if index and (seqs[index] <= seqs[index - 1] or
                      samples[index][0] <= samples[index - 1][0] or
                      samples[index][1] < samples[index - 1][1]):
            return None, "non-monotonic PC-sample ring", [], meta
    if len(samples) < 2:
        return None, f"only {len(samples)} PC sample(s) in ring", [], meta
    gaps = []
    for i in range(1, len(samples)):
        us0, cyc0, f0, _ = samples[i - 1]
        us1, cyc1, f1, x1 = samples[i]
        d_us, d_cyc = us1 - us0, cyc1 - cyc0
        if d_us <= 0:
            continue
        mcyc_s = (d_cyc / d_us) if d_us else 0.0      # cycles/us == Mcyc/s
        gaps.append({"ms": d_us / 1000.0, "guest_cyc": d_cyc,
                     "mcyc_per_s": mcyc_s,
                     "x_realtime": mcyc_s / X1_MCYC_PER_S,
                     "func_from": f0, "func_to": f1, "in_exc": x1})
    span_ms = (samples[-1][0] - samples[0][0]) / 1000.0
    gaps.sort(key=lambda g: -g["ms"])
    return span_ms, None, gaps[:top], meta


def verdict_lines(a, b):
    """The three questions, answered from the deltas."""
    out = []
    nat = delta(a, b, "overlay_loader_status", "dispatch_native")
    itp = delta(a, b, "overlay_loader_status", "dispatch_interp_fallback")
    stale = delta(a, b, "overlay_loader_status", "stale_blocked")
    tot = nat + itp
    out.append("  [1] IS THE OVERLAY CACHE BEING USED?")
    out.append(f"      dispatch_native          {nat:>14,}")
    out.append(f"      dispatch_interp_fallback {itp:>14,}")
    out.append(f"      stale_blocked            {stale:>14,}")
    out.append("      NOTE: dispatch counters are separate event counts. This")
    out.append("      report does not convert them into a coverage percentage.")
    if tot == 0:
        out.append("      >> NO DISPATCHES AT ALL in this window. Either the")
        out.append("         window caught nothing, or the loader is inactive.")
    elif nat == 0:
        out.append("      >> ZERO native dispatches were observed in this scope,")
        out.append("         even if shard files exist on disk. Check the shard")
        out.append("         counts below before reading anything into the")
        out.append("         interpreter shares - this is the boring explanation")
        out.append("         and it has to be ruled out first.")

    ac = b.get("autocompile_status", {})
    ac_compile = require_object(ac, "compile", "autocompile_status")
    ac_first = (None if a is None else
                require_object(a.get("autocompile_status", {}), "compile",
                               "autocompile_status"))
    runs = object_delta(ac_first, ac_compile, "runs", "autocompile_status.compile")
    fails = object_delta(ac_first, ac_compile, "fails", "autocompile_status.compile")
    fail_total = object_delta(ac_first, ac_compile, "shard_fail_total",
                              "autocompile_status.compile")
    configured = require_num(ac_compile, "configured", "autocompile_status.compile")
    result_seen = require_num(ac_compile, "shard_result_seen",
                              "autocompile_status.compile")
    consecutive_fails = require_num(ac_compile, "consecutive_fails",
                                    "autocompile_status.compile")
    last_exit = require_num(ac_compile, "last_exit", "autocompile_status.compile")
    shard_ok = require_num(ac_compile, "shard_ok", "autocompile_status.compile")
    shard_fail = require_num(ac_compile, "shard_fail", "autocompile_status.compile")
    shard_skipped = require_num(ac_compile, "shard_skipped",
                                "autocompile_status.compile")
    out.append("")
    out.append("      autocompile counter scope: " +
               ("since process start" if a is None else "captured delta"))
    out.append(f"      runs={runs} fails={fails} shard_fail_total={fail_total}")
    out.append("      autocompile end snapshot: "
               f"configured={configured} "
               f"state={ac_compile.get('state', '?')} "
               f"consecutive_fails={consecutive_fails} "
               f"last_exit={last_exit} "
               f"shard_ok={shard_ok} "
               f"shard_fail={shard_fail} "
               f"shard_skipped={shard_skipped} "
               f"result_seen={result_seen}")
    if require_num(ac_compile, "degraded", "autocompile_status.compile"):
        out.append("      >> DEGRADED: " + str(ac_compile.get("degraded_reason", "")))
    if not configured:
        out.append("      >> autocompile is NOT CONFIGURED. Zero compile counters")
        out.append("         are expected; only a cache built ahead of time can load.")
    elif not result_seen:
        out.append("      >> Configured, but no PSX_SHARD_RESULT has been observed.")
        out.append("         Shard counts are not evidence of a healthy compile.")
    if shard_fail > 0:
        out.append("      >> Shards are FAILING to compile. The runtime parses")
        out.append("         PSX_SHARD_RESULT from the provider, so this is the")
        out.append("         real cause, not a symptom. Run compile_overlays.py")
        out.append("         --check to see the compiler error itself.")

    dr = b.get("dirty_ram_stats", {})
    blocks = delta(a, b, "dirty_ram_stats", "blocks_run")
    insns = delta(a, b, "dirty_ram_stats", "insns_run")
    handoffs = delta(a, b, "dirty_ram_stats", "native_handoffs")
    out.append("")
    out.append("  [2] INTERPRETER DISPATCHER THRASH")
    out.append(f"      interpreted blocks       {blocks:>14,}")
    out.append(f"      interpreted instructions {insns:>14,}")
    out.append(f"      native handoffs          {handoffs:>14,}")
    if blocks:
        ratio = insns / blocks
        out.append(f"      >> {ratio:.1f} instructions per dispatcher round-trip")
        if ratio < 25:
            out.append("         That is per-block dispatch, not local chaining.")
            out.append("         Expected for the kernel window [0,0x10000), which")
            out.append("         hands control back every block by design - native")
            out.append("         coverage there comes from the overlay loader, never")
            out.append("         from interpreter chaining. If the hot PCs below are")
            out.append("         ABOVE 0x10000, that is a real bug worth reporting.")
        else:
            out.append("         Local chaining is working (overlay-region shape).")
    out.append(f"      text_native_blocked      "
               f"{require_num(dr,'text_native_blocked','dirty_ram_stats'):>14,}")
    out.append(f"      text_diverged_pages      "
               f"{require_num(dr,'text_diverged_pages','dirty_ram_stats'):>14,}")

    kb = b.get("kernel_bless", {})
    out.append(
        "      kernel_bless: "
        f"entries={require_num(kb,'entries','kernel_bless')} "
        f"clean={require_num(kb,'clean','kernel_bless')} "
        f"mismatch={require_num(kb,'mismatch','kernel_bless')} "
        f"native_hits={require_num(kb,'native_hits','kernel_bless')}")
    out.append("        (a permanent `mismatch` count is EXPECTED - runtime-")
    out.append("         patched install stubs never verify and interpret")
    out.append("         forever by design. `clean` = 0 is the odd result.)")
    return out


def hot_pc_lines(a, b, limit=12):
    """Interpreted PCs among returned rows, with aggregate accounting."""
    def per_pc(snap):
        out = {}
        for index, e in enumerate(
                (snap.get("dirty_ram_stats", {}) or {}).get("per_pc", []) or []):
            pc = e.get("pc") if isinstance(e, dict) else None
            if not isinstance(pc, str) or not pc:
                raise ReportError(f"invalid dirty_ram_stats.per_pc[{index}].pc")
            context = f"dirty_ram_stats.per_pc[{index}]"
            out[pc] = (require_num(e, "insns", context),
                       require_num(e, "hits", context),
                       require_num(e, "entries", context))
        return out
    m0, m1 = ({} if a is None else per_pc(a)), per_pc(b)
    addresses = set(m1) if a is None else set(m0) & set(m1)
    rows = []
    for pc in addresses:
        ins1, hit1, ent1 = m1[pc]
        ins0, hit0, ent0 = m0.get(pc, (0, 0, 0))
        d_ins, d_hit = ins1 - ins0, hit1 - hit0
        if d_ins < 0 or d_hit < 0:
            raise ReportError(f"dirty_ram per-PC counter reset: {pc}")
        if d_ins <= 0:
            continue
        rows.append((pc, d_ins, d_hit, (d_ins / d_hit) if d_hit else 0.0))
    rows.sort(key=lambda r: -r[1])
    accounted = sum(r[1] for r in rows)
    global_insns = delta(a, b, "dirty_ram_stats", "insns_run")
    if accounted > global_insns:
        raise ReportError("dirty_ram per-PC accounting exceeds aggregate instructions")
    unaccounted = global_insns - accounted
    out = []
    for pc, d_ins, d_hit, per in rows[:limit]:
        try:
            phys = int(pc, 16) & 0x1FFFFFFF
            zone = "KERNEL" if phys < KERNEL_WINDOW_END else "above-floor"
        except ValueError:
            zone = "?"
        out.append(f"      {pc}  insns={d_ins:>11,}  "
                   f"blocks={d_hit:>9,}  {per:6.1f} insn/entry  [{zone}]")
    out.append(f"      returned-PC accounted={accounted:,} "
               f"unaccounted={unaccounted:,} aggregate={global_insns:,}")
    out.append("      localization=" +
               ("complete" if unaccounted == 0 else "partial"))
    return out


def phase_hot_total_delta(a, b, set_name):
    key = f"phase_hot:{set_name}"
    before = None if a is None else a.get(key, {})
    after = b.get(key, {})
    return object_delta(before, after, "phase_samples_total", key)


def report(a, b, span_ms, ring_err, gaps, ring_meta=None):
    L = []
    windowed = a is not None
    L.append("=" * 78)
    if windowed:
        L.append(f"stall_report - DELTAS over a {b['wall'] - a['wall']:.1f}s window")
    else:
        L.append("stall_report - CUMULATIVE SINCE BOOT (single snapshot)")
        L.append("  For shares that are not dominated by the boot sequence, use")
        L.append("  `run --secs N` and play through the slow thing in the window.")
    L.append("=" * 78)
    L.append("")
    L.extend(verdict_lines(a, b))

    L.append("")
    L.append("  [3] WHERE THE WALL-CLOCK WENT (always-on PC-sample ring)")
    if ring_err:
        L.append(f"      ring unavailable: {ring_err}")
    else:
        ring_meta = ring_meta or {}
        L.append(f"      ring span {span_ms:.0f} ms; largest gaps:")
        L.append(f"      returned {ring_meta.get('returned', 0)} PC samples; "
                 f"requested {ring_meta.get('requested', 0)}; "
                 f"capacity {ring_meta.get('capacity', STARVATION_RING_CAP)}")
        if ring_meta.get("ring_has_wrapped"):
            L.append("      NOTE: the underlying ring wrapped. Older events are lost.")
        if ring_meta.get("request_full"):
            L.append("      NOTE: the request filled. Earlier matching samples can exist.")
        L.append("      NOTE: this scope is the last returned PC samples only.")
        L.append("        gap_ms   guest_Mcyc/s   xRT   func_from -> func_to")
        for g in gaps:
            L.append(f"      {g['ms']:8.2f}   {g['mcyc_per_s']:9.2f}   "
                     f"{g['x_realtime']:4.2f}   {g['func_from']} -> "
                     f"{g['func_to']}"
                     + ("  [in exception]" if g["in_exc"] else ""))
        L.append("      xRT = guest speed vs real hardware across that gap.")
        L.append("      HEURISTIC only. The ring does not prove a cause:")
        L.append("        ~1.0  guest execution kept pace during the gap.")
        L.append("        <<1.0 guest execution advanced slowly during the gap.")
        L.append("        ~0    the sampled emulation thread barely advanced.")
        L.append("      NOTE func is the STATIC dispatch stamp. It localizes")
        L.append("      where the thread was; it does NOT prove that code was")
        L.append("      interpreted. Use [2] for interpretation evidence.")

    pp = b.get("phase_profile", {})
    L.append("")
    phase_window_s = require_num(pp, "window_s", "phase_profile")
    L.append(f"  [4] PHASE SHARES (rolling {phase_window_s}s window, "
             "whole seconds)")
    L.append("      This window ends near the second snapshot. It is not a")
    L.append("      cumulative-since-boot or exact full-run measurement.")
    if a is not None and b["wall"] - a["wall"] > phase_window_s:
        L.append("      NOTE: the captured run exceeds this window. These shares")
        L.append("      describe only the tail of the run.")
    else:
        L.append("      NOTE: boundary seconds can include samples outside the run.")
    for k in ("interp_share", "static_share", "native_share", "gpu_share",
              "other_share", "exc_share"):
        L.append(f"      {k:<14} {require_real(pp, k, 'phase_profile')}")

    L.append("")
    L.append("  [5] HOTTEST INTERPRETED PCs (delta)")
    lines = hot_pc_lines(a, b)
    L.extend(lines or ["      (none interpreted in this window)"])

    L.append("")
    L.append("  [6] phase_hot DELTAS")
    nat = phot_delta_lines(None if a is None else a.get("phase_hot:native", {}),
                           b.get("phase_hot:native", {}))
    sta = phot_delta_lines(None if a is None else a.get("phase_hot:static", {}),
                           b.get("phase_hot:static", {}))
    L.append("    native set:")
    native_total = phase_hot_total_delta(a, b, "native")
    if nat:
        L.extend(nat)
    elif native_total == 0:
        L.append("      (no native phase samples in this captured scope)")
    else:
        L.append(f"      (localization unavailable for {native_total} native samples)")
    warning = hot_set_warning(b.get("phase_hot:native", {}),
                              b.get("phase_hot_requested_top", PHASE_HOT_TOP))
    if warning:
        L.append(warning)
    L.append("    static set:")
    static_total = phase_hot_total_delta(a, b, "static")
    if sta:
        L.extend(sta)
    elif static_total == 0:
        L.append("      (no static phase samples in this captured scope)")
    else:
        L.append(f"      (localization unavailable for {static_total} static samples)")
    warning = hot_set_warning(b.get("phase_hot:static", {}),
                              b.get("phase_hot_requested_top", PHASE_HOT_TOP))
    if warning:
        L.append(warning)
    L.append("")
    L.append("=" * 78)
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True,
                    help="the title's debug-server port (PSX_DEBUG_TOOLS=ON)")
    ap.add_argument("--out", default=None,
                    help="also write the raw snapshots + gaps as JSON here")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("snap", help="one-shot, cumulative since boot")
    r = sub.add_parser("run", help="two snapshots N seconds apart (windowed)")
    r.add_argument("--secs", type=float, default=60.0)
    args = ap.parse_args()

    c = Client(args.host, args.port)
    ping = c.cmd("ping")
    if not ping.get("ok", False):
        print(f"cannot reach debug server on {args.host}:{args.port} "
              f"({ping.get('error')}).\n"
              "A Release build ships no TCP server - rebuild with "
              "PSX_DEBUG_TOOLS=ON.", file=sys.stderr)
        return 2

    try:
        if args.mode == "run":
            phase_window = min(PHASE_RING_MAX_WINDOW,
                               max(1, int(math.ceil(args.secs)) + 1))
            first = snapshot(c, phase_window=phase_window)
            print(f"monitoring {args.secs:.0f}s - play through the slow thing now "
                  "(nothing is being armed; this only waits)...", file=sys.stderr)
            time.sleep(args.secs)
            second = snapshot(c, phase_window=phase_window)
        else:
            # snap: no earlier snapshot, so every counter is reported cumulative.
            # Passing the same snapshot as both ends would subtract it from itself
            # and print zeros, which reads as "nothing is happening".
            first, second = None, snapshot(c)

        span_ms, ring_err, gaps, ring_meta = stalls_from_ring(c)
        text = report(first, second, span_ms, ring_err, gaps, ring_meta)
    except ReportError as exc:
        print(f"incomplete or unsafe evidence: {exc}", file=sys.stderr)
        return 3
    print(text)
    if args.out:
        tool_path = Path(__file__).resolve()
        tool_sha256 = hashlib.sha256(tool_path.read_bytes()).hexdigest()
        artifact = {"schema_version": REPORT_SCHEMA_VERSION,
                    "kind": "psxrecomp-stall-diagnostic",
                    "outcome": "diagnostic",
                    "tool": {"name": "stall_report.py",
                             "sha256": tool_sha256},
                    "mode": args.mode,
                    "first": first, "second": second,
                    "ring_span_ms": span_ms, "ring_error": ring_err,
                    "ring": ring_meta, "gaps": gaps,
                    "limitations": [
                        "diagnostic only; not qualification evidence",
                        "phase shares use a bounded rolling window",
                        "hot-entry and ring results can be censored",
                    ]}
        output = Path(args.out)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + f".tmp-{os.getpid()}")
        with temporary.open("w", encoding="utf-8", newline="\n") as f:
            json.dump(artifact, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(temporary, output)
        print(f"\nraw JSON -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
