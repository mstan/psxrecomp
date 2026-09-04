#!/usr/bin/env python3
"""Check that N probed discs form one coherent multi-disc set, and say which kind.

Used by tools/new_project_layout/setup_project.{sh,ps1} when --disc is passed
more than once. Consumes probe_disc.py JSON dumps in disc order (disc 1 first)
and answers one question: can this framework build the set today?

Two kinds of multi-disc title (see docs/MULTI_DISC.md "Two axes, not one"):

  data-only   every disc boots the same program; the later discs are just more
              data. One recompiled program covers the set.
  N-programs  each disc carries its own boot executable, so the set needs N
              statically recompiled programs selected by mounted serial.

The framework can scaffold the first kind today. It cannot build the second:
the game emitter still emits unprefixed func_XXXXXXXX / k_psx_game_dispatch
(recompiler/src/main_psx.cpp), so two programs collide at link. That is P2 of
docs/MULTI_DISC.md. This script refuses an N-programs set by name rather than
quietly scaffolding a one-program project that covers only disc 1.

Exit codes:
  0  set is coherent and data-only (or a single disc) — safe to scaffold
  2  usage error
  3  set needs N programs — not buildable yet (docs/MULTI_DISC.md P1+P2)
  4  set is incoherent (duplicate disc, mixed release, unreadable probe)
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Fields that together identify the *program* a disc boots. If these agree
# across every disc, one recompiled program covers the whole set.
#
# `serial` and `boot_exe` are deliberately NOT here. They name the DISC, not
# the program: Final Fantasy VII ships ONE byte-identical executable on three
# discs as SCUS_941.63/.64/.65 under serials SCUS-94163/64/65, so comparing
# those strings reported three programs where there is one, and refused a set
# the framework can already build. `boot_exe_sha256` is the program's own
# identity and answers the question the refusal is actually asking.
PROGRAM_FIELDS = (
    "boot_exe_sha256",
    "entry_pc",
    "load_address",
    "text_size",
    "stack_base",
)

# Per-disc identity. Recorded in disc_set.json and shown in the summary, but
# never used to decide how many programs a set needs.
DISC_IDENTITY_FIELDS = ("serial", "boot_exe")

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_NEEDS_N_PROGRAMS = 3
EXIT_INCOHERENT = 4


def load_probe(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as e:
        raise SystemExit(f"cannot read probe JSON {path}: {e}")
    except json.JSONDecodeError as e:
        raise SystemExit(f"probe JSON {path} is not valid JSON: {e}")
    if not isinstance(data, dict):
        raise SystemExit(f"probe JSON {path} is not an object")
    return data


def content_key(p: dict) -> tuple:
    """What makes two probes the same *image*.

    The TOC fingerprint alone is not enough: it is derived from the track
    layout, so two different discs of one release that happen to share a
    layout produce the same fp. Requiring the volume id and data-track size to
    agree as well keeps a legitimate sibling disc from being called a
    duplicate, while still catching the same image passed twice.
    """
    return (
        str(p.get("required_disc_fp") or ""),
        str(p.get("data_track_sha1") or ""),
        str(p.get("volume_id") or ""),
        p.get("data_track_size"),
    )


def serial_prefix(serial: str) -> str:
    """SCUS-94163 -> SCUS. The publisher/region family of the release."""
    s = str(serial or "").strip().upper()
    return s[:4] if len(s) >= 4 else s


def describe(index: int, p: dict) -> str:
    return (
        f"  disc {index}: {p.get('serial') or '<no serial>'}"
        f"  boot={p.get('boot_exe') or '?'}"
        f"  entry={p.get('entry_pc') or '?'}"
        f"  tracks={p.get('track_count', '?')}"
        f"  {p.get('cue_name') or ''}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Verify N probed discs form one buildable set."
    )
    ap.add_argument(
        "probe_json",
        nargs="+",
        help="probe_disc.py --json-out dumps, in disc order (disc 1 first)",
    )
    ap.add_argument(
        "--json-out",
        default="",
        help="write the verdict as JSON (disc_set.json)",
    )
    args = ap.parse_args()

    paths = [Path(a).expanduser() for a in args.probe_json]
    probes = [load_probe(p) for p in paths]
    count = len(probes)

    print(f"== Disc set ({count} disc{'s' if count != 1 else ''}) ==")
    for i, p in enumerate(probes, start=1):
        print(describe(i, p))

    problems: list[str] = []
    warnings: list[str] = []

    # --- every disc must have identified a program -------------------------
    for i, p in enumerate(probes, start=1):
        if not str(p.get("serial") or "").strip():
            problems.append(f"disc {i} has no serial — probe could not read SYSTEM.CNF")
        if not str(p.get("entry_pc") or "").strip():
            problems.append(f"disc {i} has no entry_pc — probe could not read the PS-X EXE")

    # --- the same disc passed twice ----------------------------------------
    seen_path: dict[str, int] = {}
    seen_content: dict[tuple, int] = {}
    for i, p in enumerate(probes, start=1):
        cue = str(p.get("cue_path") or "")
        if cue and cue in seen_path:
            problems.append(
                f"disc {i} is the same file as disc {seen_path[cue]} ({cue}) — "
                "pass each disc once"
            )
        elif cue:
            seen_path[cue] = i
        key = content_key(p)
        if key in seen_content:
            problems.append(
                f"disc {i} ({p.get('cue_name')}) is the same image as disc "
                f"{seen_content[key]} — same fingerprint, volume id and size"
            )
        else:
            seen_content[key] = i

    # A shared TOC fingerprint across discs is legal but defeats the runtime's
    # disc identity check, which is what [netplay] required_disc_fp gates on.
    fps = [str(p.get("required_disc_fp") or "") for p in probes]
    if len(probes) > 1 and len({f for f in fps if f}) == 1 and fps[0]:
        warnings.append(
            "every disc has the same TOC fingerprint — [netplay] "
            "required_disc_fp cannot tell these discs apart"
        )

    # --- mixed release (US disc 1 + PAL disc 2, etc.) ----------------------
    prefixes = {serial_prefix(p.get("serial", "")) for p in probes}
    prefixes.discard("")
    if len(prefixes) > 1:
        listed = ", ".join(
            f"disc {i} {serial_prefix(p.get('serial', '')) or '?'}"
            for i, p in enumerate(probes, start=1)
        )
        problems.append(
            f"discs are from different releases ({listed}) — a set must be one "
            "release; check you have not mixed regions"
        )

    if problems:
        print()
        sys.stdout.flush()
        print("error: this is not a coherent disc set:", file=sys.stderr)
        for m in problems:
            print(f"  - {m}", file=sys.stderr)
        return EXIT_INCOHERENT

    # --- data-only vs N-programs -------------------------------------------
    first = probes[0]

    # A missing hash must not read as "these agree". Empty compares equal to
    # empty, so a probe that failed to extract the executable would silently
    # turn N programs into one -- the exact silent-wrong-answer this gate is
    # here to prevent. Refuse to judge instead.
    missing = [i for i, p in enumerate(probes, start=1)
               if not str(p.get("boot_exe_sha256") or "").strip()]
    if missing:
        print()
        sys.stdout.flush()
        print("error: cannot tell how many programs this set needs.", file=sys.stderr)
        print(f"  disc(s) {', '.join(str(i) for i in missing)} have no "
              f"boot_exe_sha256 — the probe could not read the boot executable.",
              file=sys.stderr)
        print("  Re-probe with a current probe_disc.py; refusing to guess.",
              file=sys.stderr)
        return EXIT_INCOHERENT

    differing: list[str] = []
    for field in PROGRAM_FIELDS:
        values = {str(p.get(field) or "") for p in probes}
        if len(values) > 1:
            differing.append(field)

    track_counts = {p.get("track_count") for p in probes}
    if len(track_counts) > 1:
        warnings.append(
            "discs have different track counts "
            f"({sorted(str(t) for t in track_counts)}) — [netplay] required_tracks "
            "is a single value today and will match the boot disc only"
        )

    verdict = "single" if count == 1 else ("n-programs" if differing else "data-only")
    result = {
        "disc_count": count,
        "verdict": verdict,
        "differing_program_fields": differing,
        "warnings": warnings,
        "discs": [
            {
                "index": i,
                "cue_name": p.get("cue_name"),
                "cue_path": p.get("cue_path"),
                "serial": p.get("serial"),
                "boot_exe": p.get("boot_exe"),
                "entry_pc": p.get("entry_pc"),
                "track_count": p.get("track_count"),
                "required_disc_fp": p.get("required_disc_fp"),
            }
            for i, p in enumerate(probes, start=1)
        ],
    }
    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )

    sys.stdout.flush()
    for w in warnings:
        print(f"  warning: {w}", file=sys.stderr)
    sys.stderr.flush()

    if verdict == "n-programs":
        print()
        sys.stdout.flush()
        print(
            "error: these discs each boot their own program, which this "
            "framework cannot build yet.",
            file=sys.stderr,
        )
        print(f"  differing: {', '.join(differing)}", file=sys.stderr)
        print(
            f"  disc 1 boots {first.get('boot_exe')} at {first.get('entry_pc')}; "
            "the others do not.",
            file=sys.stderr,
        )
        print(
            "  One binary linking N recompiled programs is P2 of "
            "docs/MULTI_DISC.md, and the game emitter still emits unprefixed "
            "symbols that would collide at link.",
            file=sys.stderr,
        )
        print(
            "  Scaffold disc 1 alone (pass a single --disc) until that lands. "
            "Refusing rather than writing a one-program project that silently "
            "covers only disc 1.",
            file=sys.stderr,
        )
        return EXIT_NEEDS_N_PROGRAMS

    if verdict == "data-only":
        print()
        serials = [str(p.get("serial") or "?") for p in probes]
        if len(set(serials)) > 1:
            print(
                f"  note: {len(set(serials))} serials in this set "
                f"({', '.join(serials)}) carrying one identical executable — "
                "normal for a multi-disc title, and why the program hash and "
                "not the serial decides this."
            )
        print(
            f"  all {count} discs boot the same program "
            f"(sha256 {str(first.get('boot_exe_sha256') or '')[:12]}…) — one "
            f"program covers the set."
        )
        print(
            "  Scaffolding that program now. P1 has since landed: `discs` is a "
            "first-class config entry, the runtime builds a roster from it, "
            "and it mounts the SELECTED disc — remembered in disc_index — not "
            "just the boot disc. Choosing a disc still means choosing it "
            "before the game starts; swapping one mid-session is "
            "docs/MULTI_DISC.md P3 (a faithful lid in cdrom.c), which has not "
            "landed."
        )

    return EXIT_OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SystemExit as e:
        if isinstance(e.code, int):
            raise
        print(f"error: {e}", file=sys.stderr)
        sys.exit(EXIT_INCOHERENT)
