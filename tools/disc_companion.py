"""Exact-revision SBI intake. No media payloads or network access.

The registry contains measured identities, not a region-based protection list.
See docs/DISC_COMPANIONS.md for provenance and coverage limits.
"""

from __future__ import annotations

import hashlib
from pathlib import Path


class CompanionError(ValueError):
    pass


# Key: (main-track byte size, SHA-1). Values are metadata, never SBI records.
REQUIRED_SBI = {
    (712491360, "b3ec6631e1c9ed3b0f554b3a5f61166bac8ebb9d"): {
        "title": "Resident Evil 3: Nemesis",
        "serial": "SLES-02529",
        "revision": "Europe, data-track SHA-1 b3ec6631e1c9ed3b0f554b3a5f61166bac8ebb9d",
        "sbi_sha256": "ada8877a2a964eff2743d53cc043be0b7b148469b60de82fd1069f32700670eb",
        "evidence": "RE3 same-executable operator acceptance, 2026-09-05; docs/DISC_COMPANIONS.md",
    },
}


def validate_sbi(data: bytes) -> int:
    """Check the nonempty type-1 format accepted by ISOReader::LoadSBICompanion."""
    if data[:4] != b"SBI\0" or len(data) < 18 or (len(data) - 4) % 14:
        raise CompanionError("Invalid SBI: expected a header and complete type-1 records.")
    frames: set[int] = set()
    for offset in range(4, len(data), 14):
        record = data[offset:offset + 14]
        if record[3] != 1 or any((b & 15) > 9 or (b >> 4) > 9 for b in record[:3]):
            raise CompanionError("Invalid SBI: unsupported record type or invalid BCD position.")
        mm, ss, ff = [(b >> 4) * 10 + (b & 15) for b in record[:3]]
        frame = mm * 4500 + ss * 75 + ff
        if ss >= 60 or ff >= 75 or frame < 150 or frame in frames:
            raise CompanionError("Invalid SBI: out-of-range or duplicate disc position.")
        frames.add(frame)
    return len(frames)


def inspect_companion(image: Path, size: int, sha1: str) -> tuple[dict, bytes | None]:
    """Inspect only the selected image's companion; never guess from another file.

    CUE input uses the CUE basename, matching the runtime mount contract.
    Case-insensitive discovery permits .SBI on case-sensitive source volumes.
    """
    image = image.resolve()
    expected = image.with_suffix(".sbi")
    requirement = REQUIRED_SBI.get((size, sha1.lower()))
    matches = [p for p in image.parent.iterdir() if p.name.casefold() == expected.name.casefold()]
    if len(matches) > 1:
        raise CompanionError(f"Ambiguous SBI companions for {image}: {matches}. Keep one matching file.")
    report = {
        "status": "missing" if requirement else "unknown",
        "required": bool(requirement),
        "revision": requirement,
        "expected_path": str(expected),
    }
    if not matches:
        if requirement:
            raise CompanionError(
                f"Missing SBI for {requirement['title']} ({requirement['serial']}, "
                f"{requirement['revision']}). Supply your lawfully obtained matching file at "
                f"{expected}, then retry. A matching main-track hash does not include subchannel data. "
                "Setup does not supply or download SBI files."
            )
        return report, None
    companion = matches[0]
    try:
        # A type-1 SBI cannot exceed one record per addressable BCD disc frame.
        if companion.stat().st_size > 63000004:
            raise CompanionError(f"SBI exceeds the supported size: {companion}")
        data = companion.read_bytes()
    except OSError as exc:
        raise CompanionError(f"Cannot read SBI companion {companion}: {exc}") from exc
    count = validate_sbi(data)
    digest = hashlib.sha256(data).hexdigest()
    if requirement and digest != requirement["sbi_sha256"]:
        raise CompanionError(
            f"SBI identity is not qualified for {requirement['serial']} ({requirement['revision']}): "
            f"{companion}. Expected SHA-256 {requirement['sbi_sha256']}; got {digest}. "
            "Supply the matching lawful companion or ask the port maintainer to qualify this variant."
        )
    report.update(status="verified" if requirement else "format_valid_unqualified",
                  path=str(companion), sha256=digest, size=len(data), records=count)
    return report, data


def check_destination(cue: Path, data: bytes | None) -> None:
    """Refuse stale or conflicting companions before preparation writes media."""
    expected = cue.with_suffix(".sbi")
    if not expected.parent.exists():
        return
    matches = [p for p in expected.parent.iterdir() if p.name.casefold() == expected.name.casefold()]
    if len(matches) > 1:
        raise CompanionError(f"Ambiguous output SBI companions for {cue}. Keep only {expected}.")
    for path in matches:
        if data is None or not path.is_file() or path.read_bytes() != data:
            raise CompanionError(f"Conflicting output SBI: {path}. Use an empty output directory or move this file, then retry.")
        if path.name != expected.name and not expected.exists():
            raise CompanionError(f"Output SBI uses a different filename case: {path}. Rename it to {expected}, then retry.")


def stage_companion(cue: Path, report: dict, data: bytes | None) -> dict:
    check_destination(cue, data)
    result = dict(report)
    if data is not None:
        destination = cue.with_suffix(".sbi")
        if not destination.exists():
            # Exclusive creation protects a companion installed since preflight.
            with destination.open("xb") as stream:
                stream.write(data)
        result["output_path"] = str(destination.resolve())
    return result
