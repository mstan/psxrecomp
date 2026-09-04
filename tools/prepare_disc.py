#!/usr/bin/env python3
"""Normalize a PS1 disc dump to MODE2/2352 .bin/.cue for psxrecomp games.

Framework tool — game identity comes from ``game.toml`` (``[game]`` + optional
``[prepare_disc]``) or CLI flags.

Accepted inputs:
  * Redump / raw MODE2/2352 ``.bin`` (+ ``.cue``), including multi-track
    Redump sets (data Track 01 + AUDIO tracks) — the full cue/bin set is
    preserved so CDDA survives into ``out_dir``
  * Cooked 2048-byte/sector ``.iso`` (RomM-style libraries)
  * 2448-byte/sector raw dumps (legacy MotK ISO conversion): trim to 2352

Writes working ``.bin`` + ``.cue`` (or copies a multi-track Redump set),
extracts ``SYSTEM.CNF`` + boot EXE, and prints ``RESULT_CUE=<abs path>``
for the first-run wizard / RetComM.

ISO→2352 sets Mode2 Form1 sync/header/subheader/EDC (ECC zeroed — fine for
software readers). Rebuilt images are not bit-identical to Redump.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

DST_SEC = 2352
SRC_2448 = 2448
USER = 2048
USER_OFF = 24
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])


def _configure_stdio() -> None:
    """Avoid UnicodeEncodeError on Windows consoles (cp1252 / Store Python)."""
    for stream in (sys.stdout, sys.stderr):
        reconf = getattr(stream, "reconfigure", None)
        if callable(reconf):
            try:
                reconf(errors="replace")
            except Exception:  # noqa: BLE001 — best-effort stdio harden
                pass


def _build_edc_table() -> list[int]:
    table = []
    for i in range(256):
        x = i
        for _ in range(8):
            x = (x >> 1) ^ 0xD8018001 if (x & 1) else (x >> 1)
        table.append(x & 0xFFFFFFFF)
    return table


_EDC_TABLE = _build_edc_table()


def _adjust_edc(buf: bytearray, start: int, length: int) -> None:
    x = 0
    end = start + length
    for i in range(start, end):
        x = _EDC_TABLE[(x ^ buf[i]) & 0xFF] ^ (x >> 8)
    struct.pack_into("<I", buf, end, x & 0xFFFFFFFF)


def encode_mode2_form1(user: bytes, lba: int, submode: int = 0x08) -> bytes:
    if len(user) != USER:
        raise ValueError("user data must be 2048 bytes")
    sector = bytearray(DST_SEC)
    sector[0:12] = SYNC
    t = lba + 150
    mm, ss, ff = t // (75 * 60), (t // 75) % 60, t % 75
    sector[12] = ((mm // 10) << 4) | (mm % 10)
    sector[13] = ((ss // 10) << 4) | (ss % 10)
    sector[14] = ((ff // 10) << 4) | (ff % 10)
    sector[15] = 0x02
    sub = bytes([0x00, 0x00, submode & 0xFF, 0x00])
    sector[16:20] = sub
    sector[20:24] = sub
    sector[24 : 24 + USER] = user
    _adjust_edc(sector, 0x10, 0x800 + 8)
    return bytes(sector)


@dataclass
class KnownImage:
    size: int = 0
    md5: str = ""
    sha1: str = ""


@dataclass
class PrepareConfig:
    project_root: Path
    out_dir: Path
    bin_name: str
    cue_name: str
    boot_exe: str
    serial: str = ""
    known: list[KnownImage] = field(default_factory=list)
    skip_hash_check: bool = False


def file_hashes(path: Path) -> tuple[str, str, int]:
    h_md5 = hashlib.md5()
    h_sha1 = hashlib.sha1()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            h_md5.update(chunk)
            h_sha1.update(chunk)
    return h_md5.hexdigest(), h_sha1.hexdigest(), size


def _parse_array_items(inner: str) -> list[object]:
    items: list[object] = []
    if not inner.strip():
        return items
    for part in re.split(r",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)", inner):
        part = part.strip().rstrip(",").strip()
        if not part:
            continue
        if part.startswith('"') and part.endswith('"'):
            items.append(part[1:-1])
        else:
            try:
                items.append(int(part, 0))
            except ValueError:
                items.append(part)
    return items


def _parse_toml_simple(text: str) -> dict[str, dict[str, str]]:
    """Minimal TOML reader for flat string/int/bool + string/int arrays.

    Supports multi-line arrays (``key = [ … ]`` spanning lines).
    """
    sections: dict[str, dict[str, object]] = {"": {}}
    cur = ""
    array_key: str | None = None
    array_buf: list[str] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if array_key is not None:
            array_buf.append(line)
            joined = " ".join(array_buf)
            if "]" in joined:
                inner = joined.split("]", 1)[0]
                # Drop leading "[" already consumed when array opened
                if inner.startswith("["):
                    inner = inner[1:]
                sections[cur][array_key] = _parse_array_items(inner)
                array_key = None
                array_buf = []
            continue
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1].strip()
            sections.setdefault(cur, {})
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if val.startswith("["):
            if val.endswith("]") and val.count("[") == val.count("]"):
                sections[cur][key] = _parse_array_items(val[1:-1])
            else:
                array_key = key
                array_buf = [val[1:]]  # after opening [
            continue
        if val.startswith('"') and val.endswith('"'):
            sections[cur][key] = val[1:-1]
        elif val.lower() in ("true", "false"):
            sections[cur][key] = val.lower() == "true"
        else:
            try:
                sections[cur][key] = int(val, 0)
            except ValueError:
                sections[cur][key] = val
    return sections  # type: ignore[return-value]


def load_config(project_root: Path, config_path: Path | None, out_dir_cli: str | None,
                skip_hash: bool) -> PrepareConfig:
    project_root = project_root.resolve()
    game: dict = {}
    prep: dict = {}
    if config_path and config_path.is_file():
        sections = _parse_toml_simple(config_path.read_text(encoding="utf-8"))
        game = sections.get("game", {}) or {}
        prep = sections.get("prepare_disc", {}) or {}

    boot = str(prep.get("boot_exe") or "")
    if not boot:
        exe = str(game.get("exe") or "")
        boot = Path(exe).name if exe else ""
    if not boot:
        boot = "SLUS_000.00"

    out_rel = str(prep.get("out_dir") or "prepared_disc")
    out_dir = Path(out_dir_cli).expanduser() if out_dir_cli else (project_root / out_rel)

    disc = str(game.get("disc") or prep.get("bin_name") or "game.bin")
    bin_name = str(prep.get("bin_name") or Path(disc).name)
    cue_name = str(prep.get("cue_name") or (Path(bin_name).stem + ".cue"))

    known: list[KnownImage] = []
    sizes = prep.get("known_sizes") or []
    md5s = prep.get("known_md5") or []
    sha1s = prep.get("known_sha1") or []
    if isinstance(sizes, list) or isinstance(md5s, list) or isinstance(sha1s, list):
        n = max(
            len(sizes) if isinstance(sizes, list) else 0,
            len(md5s) if isinstance(md5s, list) else 0,
            len(sha1s) if isinstance(sha1s, list) else 0,
        )
        for i in range(n):
            known.append(
                KnownImage(
                    size=int(sizes[i]) if isinstance(sizes, list) and i < len(sizes) else 0,
                    md5=str(md5s[i]).lower()
                    if isinstance(md5s, list) and i < len(md5s)
                    else "",
                    sha1=str(sha1s[i]).lower()
                    if isinstance(sha1s, list) and i < len(sha1s)
                    else "",
                )
            )

    return PrepareConfig(
        project_root=project_root,
        out_dir=out_dir.resolve(),
        bin_name=bin_name,
        cue_name=cue_name,
        boot_exe=boot,
        serial=str(game.get("id") or ""),
        known=known,
        skip_hash_check=skip_hash,
    )


def list_cue_bins(cue_path: Path) -> list[Path]:
    """Return every BINARY FILE referenced by a cue, in order."""
    text = cue_path.read_text(encoding="utf-8", errors="replace")
    names = re.findall(r'FILE\s+"([^"]+)"\s+BINARY', text, flags=re.I)
    if not names:
        names = re.findall(r"FILE\s+(\S+)\s+BINARY", text, flags=re.I)
    if not names:
        raise SystemExit(f"no BINARY FILE in cue: {cue_path}")
    out: list[Path] = []
    for name in names:
        cand = Path(name)
        if not cand.is_absolute():
            cand = cue_path.parent / cand
        if not cand.is_file():
            raise SystemExit(f"cue references missing bin: {cand}")
        out.append(cand.resolve())
    return out


def resolve_cue_bin(cue_path: Path) -> Path:
    """First BINARY file in the cue (data track for Redump multi-track sets)."""
    return list_cue_bins(cue_path)[0]


def stage_multitrack_cue(
    cue_src: Path, bins: list[Path], out_dir: Path, cue_name: str
) -> Path:
    """Copy a multi-track Redump cue + all bin files into out_dir unchanged."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for bin_src in bins:
        dest = out_dir / bin_src.name
        if dest.resolve() == bin_src.resolve():
            print(f"source already at {dest}")
            continue
        print(f"copying {bin_src.name} -> {dest}")
        shutil.copy2(bin_src, dest)
    cue_dest = out_dir / cue_name
    if cue_dest.resolve() != cue_src.resolve():
        # Rewrite FILE lines to basenames so the cue stays portable if the
        # source used absolute/relative paths with directories.
        text = cue_src.read_text(encoding="utf-8", errors="replace")
        def _basename_file(m: re.Match[str]) -> str:
            return f'FILE "{Path(m.group(1)).name}" BINARY'
        text = re.sub(
            r'FILE\s+"([^"]+)"\s+BINARY', _basename_file, text, flags=re.I
        )
        # NB: Path.write_text(newline=) is Python 3.10+. Use open() so the
        # tools keep working on 3.9, which RHEL/Rocky 9, Debian 11 and Ubuntu
        # 20.04 still ship as the system python3.
        with open(cue_dest, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print(f"wrote {cue_dest}")
    else:
        print(f"source cue already at {cue_dest}")
    return cue_dest.resolve()


def detect_kind(path: Path, size: int) -> str:
    suf = path.suffix.lower()
    if suf == ".cue":
        return "cue"
    if suf == ".iso":
        return "iso"
    # MotK-style 2448 before generic 2352 (some sizes divisible by both).
    if size % SRC_2448 == 0 and size > SRC_2448 and size % DST_SEC != 0:
        return "raw2448"
    if size % SRC_2448 == 0 and size > SRC_2448:
        # Peek: if sync looks like CD at +0, treat as 2448 when user data path works.
        with open(path, "rb") as f:
            head = f.read(16)
        if head[:12] == SYNC:
            # Could be 2352 or 2448; check sector stride via second sync.
            fpos_2352_ok = False
            fpos_2448_ok = False
            with open(path, "rb") as f:
                f.seek(DST_SEC)
                fpos_2352_ok = f.read(12) == SYNC
                f.seek(SRC_2448)
                fpos_2448_ok = f.read(12) == SYNC
            if fpos_2448_ok and not fpos_2352_ok:
                return "raw2448"
    if size % DST_SEC == 0 and size > DST_SEC:
        with open(path, "rb") as f:
            sec0 = f.read(12)
            f.seek(16 * DST_SEC)
            sync16 = f.read(12)
        if sec0 == SYNC or sync16 == SYNC:
            return "bin2352"
    if size % USER == 0 and size > USER:
        return "iso"
    if size % SRC_2448 == 0 and size > SRC_2448:
        return "raw2448"
    raise SystemExit(
        f"unrecognized disc image {path} (size={size}). "
        "Need MODE2/2352 .bin/.cue, cooked 2048 .iso, or 2448-byte/sector dump."
    )


def read_user_bin(data: bytes, lba: int) -> bytes:
    off = lba * DST_SEC
    if off + DST_SEC > len(data):
        raise KeyError(lba)
    if data[off : off + 12] != SYNC:
        raise SystemExit(f"bad sync at LBA {lba}")
    return data[off + USER_OFF : off + USER_OFF + USER]


def read_user_iso(data: bytes, lba: int) -> bytes:
    off = lba * USER
    if off + USER > len(data):
        raise KeyError(lba)
    return data[off : off + USER]


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


def extract_via(
    read_user, data: bytes, boot_exe: str
) -> tuple[dict[str, tuple[int, int]], dict[str, bytes]]:
    pvd = read_user(data, 16)
    if pvd[1:6] != b"CD001":
        raise SystemExit("PVD not found at LBA 16 (expected CD001)")
    root_extent = struct.unpack_from("<I", pvd, 158)[0]
    root_size = struct.unpack_from("<I", pvd, 166)[0]
    root = bytearray()
    for i in range((root_size + USER - 1) // USER):
        root += read_user(data, root_extent + i)
    root = bytes(root[:root_size])
    entries = parse_root_entries(root)
    files: dict[str, bytes] = {}
    # Very early titles (e.g. King's Field, Dec 1994) ship no SYSTEM.CNF; the
    # BIOS falls back to booting PSX.EXE from the root directory. probe_disc.py
    # already accepts these discs (5ab7a053); staging has to accept the same
    # ones or a clean worktree can never prepare them.
    needed = ["SYSTEM.CNF", boot_exe]
    if "SYSTEM.CNF" not in entries:
        if boot_exe not in entries:
            raise SystemExit(
                f"SYSTEM.CNF missing on disc and no {boot_exe} fallback "
                f"(found {sorted(entries)[:20]})"
            )
        print(
            "  SYSTEM.CNF missing; using the BIOS "
            f"{boot_exe} fallback boot path"
        )
        needed = [boot_exe]
    for need in needed:
        if need not in entries:
            raise SystemExit(f"missing {need} on disc (found {sorted(entries)[:20]})")
        extent, size = entries[need]
        out = bytearray()
        rem, lba = size, extent
        while rem > 0:
            sector = read_user(data, lba)
            take = min(USER, rem)
            out += sector[:take]
            rem -= take
            lba += 1
        files[need] = bytes(out)
    if files[boot_exe][:8] != b"PS-X EXE":
        raise SystemExit(f"{boot_exe} is not a PS-X EXE")
    return entries, files


def iso_to_bin(iso_data: bytes) -> bytes:
    if len(iso_data) % USER != 0:
        raise SystemExit(f"ISO size {len(iso_data)} is not a multiple of 2048")
    n = len(iso_data) // USER
    out = bytearray()
    for lba in range(n):
        user = iso_data[lba * USER : (lba + 1) * USER]
        out += encode_mode2_form1(user, lba, submode=0x08)
    return bytes(out)


def raw2448_to_bin(src: Path) -> bytes:
    out = bytearray()
    n = 0
    with open(src, "rb") as inf:
        while True:
            sec = inf.read(SRC_2448)
            if not sec:
                break
            if len(sec) != SRC_2448:
                raise SystemExit(f"truncated 2448 sector {n}: got {len(sec)} bytes")
            out += sec[:DST_SEC]
            n += 1
    print(f"  trimmed {n} sectors 2448 -> 2352")
    return bytes(out)


def matches_known(cfg: PrepareConfig, size: int, md5: str, sha1: str) -> bool:
    if not cfg.known:
        return False
    md5 = md5.lower()
    sha1 = sha1.lower()
    for k in cfg.known:
        if k.md5 and k.md5.lower() == md5:
            if not k.size or k.size == size:
                return True
        if k.sha1 and k.sha1.lower() == sha1:
            if not k.size or k.size == size:
                return True
    return False


def main() -> int:
    _configure_stdio()
    here = Path(__file__).resolve().parent
    # tools/ -> psxrecomp/ -> often game root is parent of psxrecomp
    fw_root = here.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", nargs="?", default="", help="path to .iso / .cue / .bin")
    ap.add_argument(
        "--config",
        default="",
        help="game.toml (reads [game] + [prepare_disc])",
    )
    ap.add_argument(
        "--project-root",
        default="",
        help="game project root (default: parent of config, else cwd)",
    )
    ap.add_argument("--out-dir", default="", help="override prepare_disc.out_dir")
    ap.add_argument("--bin-name", default="", help="override output .bin basename")
    ap.add_argument("--cue-name", default="", help="override output .cue basename")
    ap.add_argument("--boot-exe", default="", help="override boot EXE name on disc")
    ap.add_argument(
        "--skip-hash-check",
        action="store_true",
        help="do not require prepare_disc.known_* digests",
    )
    args = ap.parse_args()

    config_path = Path(args.config).expanduser() if args.config else None
    if config_path and not config_path.is_file():
        print(f"config not found: {config_path}", file=sys.stderr)
        return 1

    if args.project_root:
        project_root = Path(args.project_root).expanduser().resolve()
    elif config_path:
        project_root = config_path.parent.resolve()
    else:
        # Invoked from a game tree: prefer cwd if it has game.toml
        cwd = Path.cwd()
        project_root = cwd if (cwd / "game.toml").is_file() else fw_root.parent

    if not config_path:
        cand = project_root / "game.toml"
        if cand.is_file():
            config_path = cand

    cfg = load_config(project_root, config_path, args.out_dir or None, args.skip_hash_check)
    if args.bin_name:
        cfg.bin_name = args.bin_name
    if args.cue_name:
        cfg.cue_name = args.cue_name
    if args.boot_exe:
        cfg.boot_exe = args.boot_exe

    if not args.source:
        # Convenience: existing working bin under out_dir
        cand = cfg.out_dir / cfg.bin_name
        if cand.is_file():
            args.source = str(cand)
        else:
            print("source required (.iso / .cue / .bin)", file=sys.stderr)
            return 1

    src = Path(args.source).expanduser().resolve()
    if not src.is_file():
        print(f"source not found: {src}", file=sys.stderr)
        return 1

    cfg.out_dir.mkdir(parents=True, exist_ok=True)

    cue_src: Path | None = None
    cue_bins: list[Path] = []
    if src.suffix.lower() == ".cue":
        cue_src = src.resolve()
        cue_bins = list_cue_bins(cue_src)
        print(f"source cue: {cue_src} ({len(cue_bins)} BINARY file(s))")
        src = cue_bins[0]
        print(f"data track: {src.name}")

    src_md5, src_sha1, src_size = file_hashes(src)
    print(f"source: {src}")
    print(f"  size  {src_size}")
    print(f"  md5   {src_md5}")
    print(f"  sha1  {src_sha1}")
    if cfg.serial:
        print(f"  game  {cfg.serial} boot={cfg.boot_exe}")

    kind = detect_kind(src, src_size)

    known_hit = matches_known(cfg, src_size, src_md5, src_sha1)
    if cfg.known and not cfg.skip_hash_check:
        if known_hit:
            print("  known digest OK")
        elif kind in ("bin2352", "raw2448"):
            print(
                "source digests are not in prepare_disc.known_* "
                "(pass --skip-hash-check to force)",
                file=sys.stderr,
            )
            return 1
        else:
            print(
                "ISO digests are not in prepare_disc.known_*; "
                "verifying boot EXE only."
            )
    elif not cfg.known and not cfg.skip_hash_check and kind == "bin2352":
        print("  no prepare_disc.known_* configured - verifying boot EXE only")

    # Multi-track Redump: keep the cue + every track bin so CDDA works.
    if cue_src is not None and len(cue_bins) > 1 and kind == "bin2352":
        print(f"  preserving multi-track Redump set ({len(cue_bins)} files)")
        bin_data = src.read_bytes()
        entries, files = extract_via(read_user_bin, bin_data, cfg.boot_exe)
        print(f"  root entries: {sorted(entries)[:24]}")
        pc0 = struct.unpack_from("<I", files[cfg.boot_exe], 0x10)[0]
        print(f"  {cfg.boot_exe} PC0={pc0:#010x} ({len(files[cfg.boot_exe])} bytes)")
        for name, blob in files.items():
            out_path = cfg.out_dir / name
            out_path.write_bytes(blob)
            print(f"wrote {out_path} ({len(blob)} bytes)")
        cue_path = stage_multitrack_cue(
            cue_src, cue_bins, cfg.out_dir, cfg.cue_name
        )
        out_md5, out_sha1, out_size = file_hashes(cfg.out_dir / src.name)
        print("working data track:")
        print(f"  size  {out_size}")
        print(f"  md5   {out_md5}")
        print(f"  sha1  {out_sha1}")
        print(f"RESULT_CUE={cue_path}")
        return 0

    if kind == "bin2352":
        bin_data = src.read_bytes()
        entries, files = extract_via(read_user_bin, bin_data, cfg.boot_exe)
    elif kind == "iso":
        iso_data = src.read_bytes()
        entries, files = extract_via(read_user_iso, iso_data, cfg.boot_exe)
        print(f"  converting {len(iso_data) // USER} sectors ISO -> MODE2/2352...")
        bin_data = iso_to_bin(iso_data)
        extract_via(read_user_bin, bin_data, cfg.boot_exe)
    elif kind == "raw2448":
        print("  converting 2448 -> 2352...")
        bin_data = raw2448_to_bin(src)
        entries, files = extract_via(read_user_bin, bin_data, cfg.boot_exe)
    else:
        print(f"unsupported kind {kind}", file=sys.stderr)
        return 1

    print(f"  root entries: {sorted(entries)[:24]}")
    pc0 = struct.unpack_from("<I", files[cfg.boot_exe], 0x10)[0]
    print(f"  {cfg.boot_exe} PC0={pc0:#010x} ({len(files[cfg.boot_exe])} bytes)")

    for name, blob in files.items():
        out_path = cfg.out_dir / name
        out_path.write_bytes(blob)
        print(f"wrote {out_path} ({len(blob)} bytes)")

    bin_path = cfg.out_dir / cfg.bin_name
    if src.resolve() == bin_path.resolve() and kind == "bin2352":
        print(f"source already at {bin_path}")
    else:
        print(f"writing {bin_path}")
        bin_path.write_bytes(bin_data)

    cue_path = cfg.out_dir / cfg.cue_name
    with open(cue_path, "w", encoding="ascii", newline="\n") as fh:
        fh.write(
            f'FILE "{cfg.bin_name}" BINARY\n'
            f"  TRACK 01 MODE2/2352\n"
            f"    INDEX 01 00:00:00\n"
        )
    print(f"wrote {cue_path}")

    out_md5, out_sha1, out_size = file_hashes(bin_path)
    print("working bin:")
    print(f"  size  {out_size}")
    print(f"  md5   {out_md5}")
    print(f"  sha1  {out_sha1}")
    if kind == "iso":
        print("  rebuilt from ISO (EDC set; ECC zeroed - fine for software readers)")
    elif kind == "raw2448":
        print("  trimmed from 2448-byte/sector dump")

    print(f"RESULT_CUE={cue_path.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
