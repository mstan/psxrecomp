#!/usr/bin/env python3
"""Probe a Redump-style PS1 .cue (+ bins) and emit identity for game.toml / catalog.

Used by tools/new_project_layout/setup_project.{sh,ps1}. Prefer probing the
source cue in place (external dumps); optionally extract only the boot EXE
with ``--write-boot-exe``. Full cue+bin staging into ``disc/`` is optional
(``setup_project --stage-disc``).

Writes:
  - JSON to stdout (or --json-out)
  - Optional game.toml (--write-game-toml)
  - Optional catalog_identity.json (--write-catalog)
  - Optional seeds/ghidra_funcs.txt (--write-seeds): entry + in-image JAL targets
  - Optional boot EXE file (--write-boot-exe DIR)

Does not require a pre-filled game.toml. Derives serial / boot EXE from
SYSTEM.CNF, EXE header fields from the PS-X EXE, digests from data Track 01,
track count + TOC fingerprint from the cue sheet, and a first-pass seed list
from the boot EXE text.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path

DST_SEC = 2352
USER = 2048
USER_OFF = 24
EXE_HDR = 0x800
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])


@dataclass
class CueTrack:
    number: int
    kind: str  # "AUDIO" or MODE…
    file: str
    index01_frames: int | None = None
    index00_frames: int | None = None


@dataclass
class DiscProbe:
    cue_path: str
    cue_name: str
    bin_name: str
    track_count: int
    data_track_size: int
    data_track_md5: str
    data_track_sha1: str
    data_track_crc32: str = ""  # 8-digit lowercase hex (Redump-compatible)
    boot_exe: str = ""
    serial: str = ""  # SLUS-00562
    serial_exe: str = ""  # SLUS_005.62
    volume_id: str = ""
    display_name: str = ""
    load_address: str = ""
    entry_pc: str = ""
    text_size: str = ""
    stack_base: str = ""
    require_cue: bool = True
    required_disc_fp: str = ""
    # Content hash of the boot executable itself. `serial` and `boot_exe` name
    # the DISC; this names the PROGRAM, and the two are not the same thing --
    # Final Fantasy VII ships one byte-identical executable on three discs
    # under three serials (SCUS-94163/64/65).
    boot_exe_sha256: str = ""
    seed_count: int = 0
    seed_addrs: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    # Not serialized — used by --write-boot-exe
    boot_exe_bytes: bytes = field(default=b"", repr=False, compare=False)


def file_hashes(path: Path) -> tuple[str, str, str, int]:
    """Return (md5_hex, sha1_hex, crc32_hex8, size)."""
    import zlib

    h_md5, h_sha1 = hashlib.md5(), hashlib.sha1()
    crc = 0
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            h_md5.update(chunk)
            h_sha1.update(chunk)
            crc = zlib.crc32(chunk, crc)
    return (
        h_md5.hexdigest(),
        h_sha1.hexdigest(),
        f"{crc & 0xFFFFFFFF:08x}",
        size,
    )


def msf_to_frames(msf: str) -> int:
    parts = msf.strip().split(":")
    if len(parts) != 3:
        raise ValueError(f"bad MSF: {msf}")
    mm, ss, ff = (int(p) for p in parts)
    return ((mm * 60) + ss) * 75 + ff


def resolve_cue_file(cue_path: Path, name: str) -> Path:
    cand = Path(name.replace("\\", "/"))
    if not cand.is_absolute():
        cand = cue_path.parent / cand
    if cand.is_file():
        return cand.resolve()
    # Case-insensitive fallback
    parent = cand.parent
    want = cand.name.lower()
    if parent.is_dir():
        for child in parent.iterdir():
            if child.is_file() and child.name.lower() == want:
                return child.resolve()
    raise SystemExit(f"cue references missing bin: {cand}")


def parse_cue(cue_path: Path) -> tuple[list[CueTrack], list[str]]:
    text = cue_path.read_text(encoding="utf-8", errors="replace")
    tracks: list[CueTrack] = []
    files_order: list[str] = []
    current_file = ""
    current: CueTrack | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("REM"):
            continue
        m = re.match(r'FILE\s+"([^"]+)"\s+(\S+)', line, re.I)
        if not m:
            m = re.match(r"FILE\s+(\S+)\s+(\S+)", line, re.I)
        if m:
            current_file = m.group(1)
            if current_file not in files_order:
                files_order.append(current_file)
            continue
        m = re.match(r"TRACK\s+(\d+)\s+(\S+)", line, re.I)
        if m:
            if current:
                tracks.append(current)
            current = CueTrack(
                number=int(m.group(1)),
                kind=m.group(2).upper(),
                file=current_file,
            )
            continue
        m = re.match(r"INDEX\s+(\d+)\s+(\d+:\d+:\d+)", line, re.I)
        if m and current:
            idx = int(m.group(1))
            frames = msf_to_frames(m.group(2))
            if idx == 0:
                current.index00_frames = frames
            elif idx == 1:
                current.index01_frames = frames
    if current:
        tracks.append(current)
    if not tracks:
        raise SystemExit(f"no TRACK lines in cue: {cue_path}")
    return tracks, files_order


def first_binary_bin(cue_path: Path, files_order: list[str], tracks: list[CueTrack]) -> Path:
    t1 = next((t for t in tracks if t.number == 1), tracks[0])
    name = t1.file or (files_order[0] if files_order else "")
    if not name:
        raise SystemExit(f"cue has no FILE for track 1: {cue_path}")
    return resolve_cue_file(cue_path, name)


def compute_disc_fp(cue_path: Path, tracks: list[CueTrack], files_order: list[str]) -> str:
    """Match runtime disc_identity.cpp psxrecomp-toc-v1 SHA-256."""
    next_lba = 0
    file_start: dict[str, int] = {}
    for name in files_order:
        path = resolve_cue_file(cue_path, name)
        size = path.stat().st_size
        raw = size % DST_SEC == 0 and size > 0
        sec = DST_SEC if raw else USER
        if size % sec != 0:
            raise SystemExit(f"track file size {size} not a multiple of {sec}: {path}")
        file_start[name] = next_lba
        next_lba += size // sec
    leadout = next_lba

    lines = [
        "psxrecomp-toc-v1\n",
        f"tracks={len(tracks)}\n",
        f"leadout={leadout}\n",
    ]
    for t in sorted(tracks, key=lambda x: x.number):
        if not t.file or t.file not in file_start:
            raise SystemExit(f"track {t.number} has no FILE mapping")
        base = file_start[t.file]
        index01 = t.index01_frames if t.index01_frames is not None else 0
        index00 = t.index00_frames if t.index00_frames is not None else index01
        start = base + index01
        pregap = base + index00
        audio = 1 if "AUDIO" in t.kind.upper() else 0
        lines.append(
            f"t={t.number},a={audio},start={start},pregap={pregap}\n"
        )
    canonical = "".join(lines)
    return hashlib.sha256(canonical.encode("ascii")).hexdigest()


def scan_jal_seeds(exe: bytes) -> list[int]:
    """Entry PC + direct JAL targets inside the boot EXE text (MotK-style)."""
    if exe[:8] != b"PS-X EXE":
        raise SystemExit("not a PS-X EXE")
    pc0 = struct.unpack_from("<I", exe, 0x10)[0]
    t_addr = struct.unpack_from("<I", exe, 0x18)[0]
    t_size = struct.unpack_from("<I", exe, 0x1C)[0]
    text = exe[EXE_HDR : EXE_HDR + t_size]
    if len(text) < 4:
        return sorted({pc0 & ~3})
    lo, hi = t_addr, t_addr + len(text)
    seeds = {pc0 & ~3}
    for off in range(0, len(text) - 3, 4):
        w = struct.unpack_from("<I", text, off)[0]
        if (w >> 26) != 3:  # JAL
            continue
        pc = t_addr + off
        target = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        if lo <= target < hi and (target & 3) == 0:
            seeds.add(target)
    return sorted(seeds)


def render_seeds_file(seeds: list[int], *, boot_exe: str, entry: int, load: int, text: int) -> str:
    lines = [
        f"# Auto-scanned JAL targets (+ entry) from {boot_exe}",
        f"# entry=0x{entry:08x} load=0x{load:08x} text_size=0x{text:x}",
        "# First-pass only — add overlay / runtime discoveries as you decomp.",
    ]
    for addr in seeds:
        lines.append(f"0x{addr:08X}")
    lines.append("")
    return "\n".join(lines)


def read_user_bin(data: bytes, lba: int) -> bytes:
    off = lba * DST_SEC
    if off + DST_SEC > len(data):
        raise KeyError(lba)
    if data[off : off + 12] != SYNC:
        off2 = lba * USER
        if off2 + USER <= len(data) and data[off2 + 1 : off2 + 6] == b"CD001":
            return data[off2 : off2 + USER]
        if lba == 16:
            raise SystemExit("bad sync at LBA 16 (expected MODE2/2352 or cooked ISO)")
        raise KeyError(lba)
    return data[off + USER_OFF : off + USER_OFF + USER]


def parse_root_entries(root: bytes) -> dict[str, tuple[int, int]]:
    entries: dict[str, tuple[int, int]] = {}
    i = 0
    while i < len(root):
        reclen = root[i]
        if reclen == 0:
            i = ((i // USER) + 1) * USER
            if i >= len(root):
                break
            continue
        if i + reclen > len(root):
            break
        extent = struct.unpack_from("<I", root, i + 2)[0]
        size = struct.unpack_from("<I", root, i + 10)[0]
        namelen = root[i + 32]
        name = root[i + 33 : i + 33 + namelen]
        if b";" in name:
            name = name.split(b";")[0]
        if name not in (b"\x00", b"\x01"):
            entries[name.decode("ascii", "replace")] = (extent, size)
        i += reclen
    return entries


def read_file(read_user, data: bytes, extent: int, size: int) -> bytes:
    out = bytearray()
    rem, lba = size, extent
    while rem > 0:
        sector = read_user(data, lba)
        take = min(USER, rem)
        out += sector[:take]
        rem -= take
        lba += 1
    return bytes(out)


def parse_system_cnf(cnf: bytes) -> str:
    text = cnf.decode("ascii", "replace")
    m = re.search(r"BOOT\s*=\s*cdrom:\\?([^;\s]+)", text, re.I)
    if not m:
        m = re.search(r"BOOT\s*=\s*([^;\s]+)", text, re.I)
    if not m:
        raise SystemExit("SYSTEM.CNF has no BOOT= line")
    token = m.group(1).strip()
    token = token.split("\\")[-1].split("/")[-1]
    return token


def normalize_serial(boot_exe: str) -> tuple[str, str]:
    """Return (game_id SLUS-00562, exe_name SLUS_005.62)."""
    raw = boot_exe.strip()
    if "_" in raw and "." in raw:
        s = "".join(c for c in raw.upper() if c.isalnum())
        if len(s) >= 9:
            return f"{s[:4]}-{s[4:9]}", raw.upper() if raw.count(".") == 1 else (
                f"{s[:4]}_{s[4:7]}.{s[7:9]}"
            )
    s = "".join(c for c in raw.upper() if c.isalnum())
    if len(s) < 9:
        return "", raw
    game_id = f"{s[:4]}-{s[4:9]}"
    exe_name = f"{s[:4]}_{s[4:7]}.{s[7:9]}"
    return game_id, exe_name


def display_name_from_cue(cue_path: Path, volume_id: str) -> str:
    stem = cue_path.stem
    name = re.sub(
        r"\s*\((USA|Europe|Japan|World|En,Fr,De,Es,It)\)\s*$",
        "",
        stem,
        flags=re.I,
    )
    return name.strip() or stem


def probe(cue_path: Path, *, identity_only: bool = False) -> DiscProbe:
    cue_path = cue_path.resolve()
    if cue_path.suffix.lower() != ".cue":
        raise SystemExit("probe_disc expects a .cue (Redump multi-track layout)")

    tracks, files_order = parse_cue(cue_path)
    bin_path = first_binary_bin(cue_path, files_order, tracks)
    if identity_only:
        md5, sha1, crc32, size = "", "", "", bin_path.stat().st_size
    else:
        md5, sha1, crc32, size = file_hashes(bin_path)

    warnings: list[str] = []
    notes: list[str] = []
    if identity_only:
        notes.append(
            "identity-only probe: data-track md5/sha1/crc32 skipped. Not usable "
            "for [prepare_disc] digests or the catalog."
        )
    if len(tracks) == 1:
        warnings.append(
            "cue has only 1 TRACK — Track-01-only dumps fail multi-track "
            "netplay/catalog gates; prefer a full Redump cue."
        )

    disc_fp = ""
    try:
        disc_fp = compute_disc_fp(cue_path, tracks, files_order)
    except SystemExit as e:
        warnings.append(f"TOC fingerprint skipped: {e}")
    except OSError as e:
        warnings.append(f"TOC fingerprint skipped: {e}")

    print(f"  reading data track {bin_path.name} ({size} bytes)…", file=sys.stderr)
    data = bin_path.read_bytes()

    def read_user_iso(d: bytes, lba: int) -> bytes:
        off = lba * USER
        if off + USER > len(d):
            raise KeyError(lba)
        return d[off : off + USER]

    cooked = (
        len(data) >= 17 * USER
        and data[16 * USER + 1 : 16 * USER + 6] == b"CD001"
        and not (
            len(data) >= 17 * DST_SEC
            and data[16 * DST_SEC : 16 * DST_SEC + 12] == SYNC
        )
    )
    if cooked:
        read_user = read_user_iso
        warnings.append(
            "data track looks like cooked 2048-byte sectors; "
            "Generate/prepare_disc will normalize to MODE2/2352."
        )
    else:
        read_user = read_user_bin

    pvd = read_user(data, 16)
    if pvd[1:6] != b"CD001":
        raise SystemExit("PVD not found at LBA 16 (expected CD001)")

    vol = pvd[40:72].decode("ascii", "replace").strip()
    root_extent = struct.unpack_from("<I", pvd, 158)[0]
    root_size = struct.unpack_from("<I", pvd, 166)[0]
    root = bytearray()
    for i in range((root_size + USER - 1) // USER):
        root += read_user(data, root_extent + i)
    entries = parse_root_entries(bytes(root[:root_size]))

    if "SYSTEM.CNF" not in entries:
        # Very early titles (e.g. King's Field, Dec 1994) ship no SYSTEM.CNF;
        # the BIOS falls back to booting PSX.EXE from the root directory.
        psx_exe = next((k for k in entries if k.upper() == "PSX.EXE"), None)
        if psx_exe is None:
            raise SystemExit(
                f"SYSTEM.CNF missing on disc and no PSX.EXE fallback "
                f"(found {sorted(entries)[:24]})"
            )
        boot_token = psx_exe
        warnings.append(
            "SYSTEM.CNF missing; using the BIOS PSX.EXE fallback boot path. "
            "No serial is recoverable from the filesystem — set game_id "
            "manually in catalog_identity.json / game.toml."
        )
    else:
        extent, fsize = entries["SYSTEM.CNF"]
        cnf = read_file(read_user, data, extent, fsize)
        boot_token = parse_system_cnf(cnf)
    serial, boot_exe = normalize_serial(boot_token)

    disc_boot = boot_token
    if disc_boot not in entries:
        if boot_exe in entries:
            disc_boot = boot_exe
        else:
            upper = {k.upper(): k for k in entries}
            if disc_boot.upper() in upper:
                disc_boot = upper[disc_boot.upper()]
            elif boot_exe.upper() in upper:
                disc_boot = upper[boot_exe.upper()]
            else:
                raise SystemExit(
                    f"boot EXE {boot_token!r} not on disc "
                    f"(found {sorted(entries)[:24]})"
                )

    bext, bsize = entries[disc_boot]
    exe = read_file(read_user, data, bext, bsize)
    if exe[:8] != b"PS-X EXE":
        raise SystemExit(f"{disc_boot} is not a PS-X EXE")

    pc0 = struct.unpack_from("<I", exe, 0x10)[0]
    t_addr = struct.unpack_from("<I", exe, 0x18)[0]
    t_size = struct.unpack_from("<I", exe, 0x1C)[0]
    s_addr = struct.unpack_from("<I", exe, 0x30)[0]
    seeds = scan_jal_seeds(exe)

    boot_name = disc_boot
    if not serial:
        serial, _ = normalize_serial(boot_name)

    display = display_name_from_cue(cue_path, vol)
    notes.append(
        "entry_pc/load_address/text_size/stack_base taken from PS-X EXE header."
    )
    notes.append(
        "seeds are first-pass boot-EXE JAL targets (+ entry); expect to grow "
        "as overlays / runtime paths are discovered."
    )
    if disc_fp:
        notes.append(
            "required_disc_fp is psxrecomp-toc-v1 (matches runtime DiscIdentity)."
        )
    else:
        notes.append("required_disc_fp unavailable; required_tracks still set.")

    return DiscProbe(
        cue_path=str(cue_path),
        cue_name=cue_path.name,
        bin_name=bin_path.name,
        track_count=len(tracks),
        data_track_size=size,
        data_track_md5=md5,
        data_track_sha1=sha1,
        data_track_crc32=crc32,
        boot_exe=boot_name,
        serial=serial,
        serial_exe=boot_name if "_" in boot_name else (
            f"{serial[:4]}_{serial[5:8]}.{serial[8:10]}" if serial else boot_name
        ),
        volume_id=vol,
        display_name=display,
        load_address=f"0x{t_addr:08X}",
        entry_pc=f"0x{pc0:08X}",
        text_size=f"0x{t_size:08X}",
        stack_base=f"0x{s_addr:08X}",
        require_cue=True,
        required_disc_fp=disc_fp,
        seed_count=len(seeds),
        seed_addrs=[f"0x{a:08X}" for a in seeds],
        warnings=warnings,
        notes=notes,
        boot_exe_bytes=exe,
        boot_exe_sha256=hashlib.sha256(exe).hexdigest() if exe else "",
    )


def toml_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def render_game_toml(p: DiscProbe, *, disc_rel: str, out_dir: str, players: int,
                     extra_discs: list[str] | None = None) -> str:
    exe_rel = f"{out_dir.rstrip('/')}/{p.boot_exe}"
    lines = [
        "# Autofilled by tools/new_project_layout/probe_disc.py from your legal dump.",
        "# Review before shipping. Runtime quirks / overlay seeds still need hand work.",
        "",
        "[game]",
        f'name = "{toml_escape(p.display_name)}"',
        f'id = "{toml_escape(p.serial)}"' if p.serial else '# id = "SLUS-XXXXX"',
        f"players = {players}",
        f'exe = "{toml_escape(exe_rel)}"',
        # One program, N images: the loader already accepts `discs`, and a set
        # verified data-only is exactly that case. `disc` stays for a single
        # image so a one-disc title is byte-identical to before.
        *(
            [f'disc = "{toml_escape(disc_rel)}"']
            if not extra_discs
            else [
                "# Verified as one program on N images (verify_disc_set.py).",
                "# Disc 1 boots; the rest are data. The runtime builds its disc",
                "# roster from this list and mounts the SELECTED one (remembered",
                "# in disc_index), so the launcher can switch discs between runs.",
                "# Swapping mid-session is MULTI_DISC.md P3 and is not implemented.",
                "discs = [",
                *[f'    "{toml_escape(d)}",' for d in [disc_rel, *extra_discs]],
                "]",
            ]
        ),
        f'load_address = "{p.load_address}"',
        f'entry_pc = "{p.entry_pc}"',
        f'text_size = "{p.text_size}"',
        f'stack_base = "{p.stack_base}"',
        "",
        "# Digests are for data Track 01 (first BINARY FILE / TRACK 01).",
        "[prepare_disc]",
        f'out_dir = "{toml_escape(out_dir)}"',
        f'bin_name = "{toml_escape(p.bin_name)}"',
        f'cue_name = "{toml_escape(p.cue_name)}"',
        f'boot_exe = "{toml_escape(p.boot_exe)}"',
        f"known_sizes = [{p.data_track_size}]",
        "known_md5 = [",
        f'  "{p.data_track_md5}",',
        "]",
        "known_sha1 = [",
        f'  "{p.data_track_sha1}",',
        "]",
        "known_crc32 = [",
        f'  "{p.data_track_crc32}",',
        "]",
        "",
        "[recompiler]",
        'seeds = "seeds/ghidra_funcs.txt"',
        'out_dir = "generated"',
        "strict = true",
        "",
        "[runtime]",
        'window_title = "' + toml_escape(p.display_name + " Recompiled") + '"',
        'memcard_dir = "saves"',
        "",
        "[video]",
        'renderer = "opengl"',
        'aspect_ratio = "4:3"',
        "",
        "[controller]",
        'default_mode = "digital"',
        "",
        "# Online mount gate: digests + track count + optional TOC fingerprint",
        "# (see docs/NETPLAY.md).",
        "[netplay]",
        f"require_cue = {str(p.require_cue).lower()}",
        f"required_tracks = {p.track_count}",
    ]
    if p.required_disc_fp:
        lines.append(f'required_disc_fp = "{p.required_disc_fp}"')
    lines.append("")
    return "\n".join(lines)


def catalog_identity(
    p: DiscProbe,
    *,
    players: int = 2,
    description: str = "",
    publisher: str = "",
    year: str = "",
    region: str = "",
) -> dict:
    rom = {
        "cue_name": p.cue_name,
        "bin_name": p.bin_name,
        "track_counts": [p.track_count],
        "require_cue": p.require_cue,
        "data_track": {
            "size": p.data_track_size,
            "md5": p.data_track_md5,
            "sha1": p.data_track_sha1,
            "crc32": p.data_track_crc32,
        },
    }
    if p.required_disc_fp:
        rom["disc_fp"] = p.required_disc_fp
    marketing = {
        "description": description or "",
        "publisher": publisher or "",
        "year": year or "",
        "region": region or "",
        "players": int(players),
    }
    return {
        "format_version": 1,
        "game": {
            "name": p.display_name,
            "id": p.serial,
            "boot_exe": p.boot_exe,
        },
        "marketing": marketing,
        "rom_identity": rom,
        "exe": {
            "load_address": p.load_address,
            "entry_pc": p.entry_pc,
            "text_size": p.text_size,
            "stack_base": p.stack_base,
        },
        "seeds": {"count": p.seed_count, "source": "boot_exe_jal"},
        "warnings": p.warnings,
        "notes": p.notes,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cue", help="path to .cue (with sibling .bin tracks)")
    ap.add_argument("--json-out", default="", help="write probe JSON here")
    ap.add_argument("--write-game-toml", default="", help="write autofilled game.toml")
    ap.add_argument(
        "--write-catalog",
        default="",
        help="write catalog_identity.json for RetComM / submission autofill",
    )
    ap.add_argument(
        "--write-seeds",
        default="",
        help="write seeds/ghidra_funcs.txt (entry + JAL targets)",
    )
    ap.add_argument(
        "--disc-rel",
        default="",
        help="game.disc path in game.toml (relative or absolute; default: disc/<cue>)",
    )
    ap.add_argument(
        "--extra-disc",
        action="append",
        default=[],
        help="another image for game.toml `discs`, repeatable, in disc order "
             "after the boot disc. Only for a set verify_disc_set.py reported "
             "as data-only — one program, N images.",
    )
    ap.add_argument(
        "--extra-disc-list",
        default="",
        help="file of additional disc paths, one per line, in disc order. "
             "Preferred over repeated --extra-disc from a shell: PS1 dump "
             "filenames contain spaces and parentheses.",
    )
    ap.add_argument(
        "--out-dir",
        default="disc",
        help="prepare_disc.out_dir / exe parent (default: disc)",
    )
    ap.add_argument(
        "--write-boot-exe",
        default="",
        help="Write extracted boot EXE into this directory (small; not full dump)",
    )
    ap.add_argument("--players", type=int, default=2, help="game.players default")
    ap.add_argument(
        "--display-name",
        default="",
        help="override display name (default: cleaned cue stem)",
    )
    ap.add_argument("--description", default="", help="marketing blurb for catalog/README")
    ap.add_argument("--publisher", default="", help="marketing publisher")
    ap.add_argument("--year", default="", help="marketing year (string ok)")
    ap.add_argument("--region", default="", help="marketing region (e.g. USA)")
    ap.add_argument(
        "--identity-only",
        action="store_true",
        help="skip the data-track md5/sha1/crc32 pass (fast identity check; "
             "not usable with --write-game-toml / --write-catalog)",
    )
    args = ap.parse_args()

    if args.identity_only and (args.write_game_toml or args.write_catalog):
        print(
            "--identity-only cannot be combined with --write-game-toml / "
            "--write-catalog: both need the data-track digests it skips.",
            file=sys.stderr,
        )
        return 2

    cue = Path(args.cue).expanduser()
    if not cue.is_file():
        print(f"cue not found: {cue}", file=sys.stderr)
        return 1

    p = probe(cue, identity_only=args.identity_only)
    if args.display_name:
        p.display_name = args.display_name

    for w in p.warnings:
        print(f"  warning: {w}", file=sys.stderr)

    disc_rel = args.disc_rel or f"{args.out_dir.rstrip('/')}/{p.cue_name}"
    extra_discs = [d for d in (args.extra_disc or []) if d.strip()]
    if args.extra_disc_list:
        lp = Path(args.extra_disc_list)
        if lp.is_file():
            extra_discs += [ln.strip()
                            for ln in lp.read_text(encoding="utf-8").splitlines()
                            if ln.strip()]

    # Drop bulky / binary fields from JSON dumps
    payload = asdict(p)
    payload.pop("seed_addrs", None)
    payload.pop("boot_exe_bytes", None)

    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps(payload, indent=2) + "\n", encoding="utf-8"
        )
    else:
        print(json.dumps({k: v for k, v in payload.items() if k != "seed_addrs"}, indent=2))

    if args.write_game_toml:
        text = render_game_toml(
            p, disc_rel=disc_rel, out_dir=args.out_dir, players=args.players,
            extra_discs=extra_discs
        )
        Path(args.write_game_toml).write_text(text, encoding="utf-8")
        print(f"  wrote {args.write_game_toml}", file=sys.stderr)

    if args.write_boot_exe:
        if not p.boot_exe_bytes:
            print("error: no boot EXE bytes to write", file=sys.stderr)
            return 1
        dest_dir = Path(args.write_boot_exe).expanduser()
        dest_dir.mkdir(parents=True, exist_ok=True)
        dest = dest_dir / p.boot_exe
        dest.write_bytes(p.boot_exe_bytes)
        print(f"  wrote boot EXE {dest} ({len(p.boot_exe_bytes)} bytes)", file=sys.stderr)

    if args.write_catalog:
        Path(args.write_catalog).write_text(
            json.dumps(
                catalog_identity(
                    p,
                    players=args.players,
                    description=args.description,
                    publisher=args.publisher,
                    year=args.year,
                    region=args.region,
                ),
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"  wrote {args.write_catalog}", file=sys.stderr)

    if args.write_seeds:
        seeds = [int(a, 16) for a in p.seed_addrs]
        entry = int(p.entry_pc, 16)
        load = int(p.load_address, 16)
        text_sz = int(p.text_size, 16)
        out = Path(args.write_seeds)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            render_seeds_file(
                seeds,
                boot_exe=p.boot_exe,
                entry=entry,
                load=load,
                text=text_sz,
            ),
            encoding="utf-8",
        )
        print(f"  wrote {out} ({len(seeds)} seeds)", file=sys.stderr)

    fp_note = f" disc_fp={p.required_disc_fp[:12]}…" if p.required_disc_fp else ""
    print(
        f"  probed {p.serial or '?'} boot={p.boot_exe} "
        f"tracks={p.track_count} entry={p.entry_pc} seeds={p.seed_count}{fp_note}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
