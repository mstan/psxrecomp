#!/usr/bin/env python3
"""Structural regression checks for bounded overlay-candidate registration.

The full loader depends on the emulator runtime and has no isolated unit-test
link seam. These checks pin the fail-closed capacity contract in both platform
branches; the normal MinGW runtime build supplies the compilation/link proof.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
LOADER = ROOT / "runtime" / "src" / "overlay_loader.c"
OVERLAY_API = ROOT / "runtime" / "include" / "overlay_api.h"
DEBUG_SERVER = ROOT / "runtime" / "src" / "debug_server.c"


def function_body(source: str, name: str, start_at: int = 0) -> str:
    match = re.search(
        rf"\b(?:static\s+)?(?:int|void|uint64_t)\s+{re.escape(name)}\s*"
        rf"\([^;]*?\)\s*\{{",
        source[start_at:],
        re.S,
    )
    if not match:
        raise AssertionError(f"missing function definition: {name}")
    start = start_at + match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unterminated function definition: {name}")


def require(pattern: str, text: str, message: str) -> None:
    if not re.search(pattern, text, re.S):
        raise AssertionError(message)


def main() -> int:
    source = LOADER.read_text(encoding="utf-8")
    overlay_api = OVERLAY_API.read_text(encoding="utf-8")
    debug = DEBUG_SERVER.read_text(encoding="utf-8")

    cap_match = re.search(
        r"^#define\s+PSX_OVERLAY_CANDIDATE_CAP\s+(\d+)\s*$",
        overlay_api, re.M)
    if not cap_match:
        raise AssertionError(
            "PSX_OVERLAY_CANDIDATE_CAP must remain an auditable integer literal")
    cap = int(cap_match.group(1))
    if cap < 17_506 or cap & (cap - 1):
        raise AssertionError(
            "PSX_OVERLAY_CANDIDATE_CAP must be power-of-two and >= 17,506, "
            f"got {cap}")
    require(r"#define\s+CAND_CAP\s+PSX_OVERLAY_CANDIDATE_CAP", source,
            "runtime candidate arrays must use the shared public capacity")
    require(r"#define\s+IDX_CAP\s+\(CAND_CAP\s*\*\s*2u\)", source,
            "candidate hash index must scale at 2x CAND_CAP")
    require(r"#define\s+IDX_MASK\s+\(IDX_CAP\s*-\s*1u\)", source,
            "candidate hash mask must track IDX_CAP")
    require(r"#define\s+RANGE_LINK_CAP\s+\(CAND_CAP\s*\*\s*8\)", source,
            "candidate range links must scale with candidate storage")
    require(r"#define\s+LAZY_ENTRY_CAP\s+\(CAND_CAP\s*\*\s*2u\)", source,
            "lazy entry index must scale at 2x CAND_CAP")
    require(r"#define\s+LAZY_ENTRY_MASK\s+\(LAZY_ENTRY_CAP\s*-\s*1u\)", source,
            "lazy entry hash mask must track LAZY_ENTRY_CAP")
    require(r"#define\s+LAZY_MAN_CAP\s+\(CAND_CAP\s*\*\s*2\)", source,
            "lazy manifest storage must scale at 2x CAND_CAP")
    require(r"#define\s+LAZY_RANGE_LINK_CAP\s+\(LAZY_MAN_CAP\s*\*\s*8\)", source,
            "lazy range links must scale with lazy manifest storage")

    register = function_body(source, "cand_register")
    require(r"if\s*\(s_cand_n\s*>=\s*CAND_CAP\)\s*\{\s*"
            r"s_cand_overflow\+\+;\s*return\s+0\s*;", register,
            "cand_register must count and reject overflow before allocation")
    require(r"\bs_cand_n\+\+", register,
            "cand_register must allocate from the bounded candidate array")
    require(r"return\s+1\s*;\s*$", register,
            "cand_register must report successful registration")
    require(r"c->crc_code\s*=\s*m->crc", register,
            "candidate CRC must always come from an authoritative manifest")
    require(r"#define\s+LOADED_PAIR_CAP\s+4096", source,
            "whole-pair registry must cover every indexed cache file")
    pair_equal = function_body(source, "manifest_pair_equal")
    for field in ("entry", "crc", "has_crc", "n", "lo", "len"):
        require(rf"\.{field}\b", pair_equal,
                f"pair equivalence must compare manifest {field}")
    pair_find = function_body(source, "loaded_pair_find")
    require(r"p->tier\s*==\s*tier", pair_find,
            "whole-pair aliases must never cross compiler tiers")
    require(r"p->provenance\s*==\s*provenance", pair_find,
            "whole-pair aliases must preserve authority/hosted/orphan provenance")
    require(r"p->pair_id\s*==\s*pair_id", pair_find,
            "whole-pair aliases must require the bound P identity")
    require(r"manifest_pair_equal", pair_find,
            "whole-pair aliases must require exact normalized manifest identity")
    require(r"MANIFEST_PROVENANCE_AMBIGUOUS", pair_find,
            "malformed/ambiguous provenance must never authorize dedup")
    pair_commit = function_body(source, "loaded_pair_commit")
    require(r"malloc", pair_commit,
            "canonical pair identity must be retained independently of mutable files")
    require(r"memcpy", pair_commit,
            "canonical pair identity must be copied at complete registration")
    require(r"!arr\[i\]\.has_crc\s*\|\|\s*arr\[i\]\.n\s*<=\s*0", source,
            "runtime must reject legacy no-CRC/empty-range manifests")
    require(r"contains_entry", source,
            "runtime must require each manifest entry inside its guarded ranges")
    require(r"arr\[j\]\.entry[^\n]*==\s*entry", source,
            "runtime must reject duplicate physical manifest entries")
    require(r"lo\s*>=\s*OVERLAY_RAM_SIZE\s*\|\|\s*"
            r"arr\[i\]\.len\[r\]\s*>\s*OVERLAY_RAM_SIZE\s*-\s*lo", source,
            "runtime must reject manifest ranges outside 2 MiB RAM")
    require(r"manifest_skip_space\(line\)", source,
            "runtime manifest grammar must accept leading whitespace like Python")
    require(r"manifest_is_space", source,
            "runtime manifest grammar must use an explicit ASCII whitespace set")
    require(r"memchr\(line,\s*'\\0',\s*line_len\)", source,
            "runtime manifest parser must reject embedded NUL bytes")
    require(r'fopen\(path,\s*"rb"\)', source,
            "Windows manifest parsing must not treat Ctrl-Z as text EOF")
    require(r"MANIFEST_PHYSICAL_LINE_MAX\s*\+\s*1u", source,
            "runtime must bound the same physical-line width as Python")
    require(r"manifest_hex_field\(&cursor,\s*16", source,
            "pair ids must use the strict bounded manifest hex parser")
    if len(re.findall(r"manifest_hex_field\(&cursor,\s*8", source)) < 4:
        raise AssertionError(
            "F/R fields must all use the strict bounded manifest hex parser")
    require(r"manifest_record_end\(cursor\)", source,
            "runtime manifest records must reject trailing tokens like Python")
    require(r"#ifdef\s+_WIN32[^#]*_stricmp\(a,\s*b\)", source,
            "Windows tier dedup must use case-insensitive cache basenames")
    if len(re.findall(r"registered\s*\+=\s*cand_register\s*\(", source)) != 2:
        raise AssertionError("both platform loaders must count cand_register success honestly")

    suppress = function_body(source, "cache_entry_suppress_at_capacity")
    require(r"s_cand_n\s*<\s*CAND_CAP", suppress,
            "capacity suppression must occur only at the permanent full state")
    require(r"cache_entry_suppress_for_shortfall", suppress,
            "full-cap suppression must share the durable shortfall path")
    shortfall = function_body(source, "cache_entry_suppress_for_shortfall")
    require(r"needed\s*<=\s*CAND_CAP\s*-\s*s_cand_n", shortfall,
            "a bundle must be suppressed only when it cannot fit atomically")
    require(r"e->capacity_suppressed\s*=\s*1", shortfall,
            "oversized bundles must be memoized to prevent loader retry churn")
    require(r"overlay_image_warm_drop_all\s*\(\)", shortfall,
            "permanent capacity exhaustion must release speculative DLL mappings")
    require(r"overlay_image_warm_cancel\s*\(ci\)", shortfall,
            "near-cap shortfall must cancel that bundle's speculative mapping")
    require(r"overlay_image_warm_release\s*\(ci\)", shortfall,
            "near-cap shortfall must release that bundle's speculative mapping")
    warm_queue = function_body(source, "overlay_image_warm_queue")
    require(r"s_capacity_warm_dropped\s*\|\|\s*"
            r"s_cand_n\s*>=\s*CAND_CAP", warm_queue,
            "warm queue must stay disabled after permanent capacity exhaustion")
    require(r"s_cache_idx\[ci\]\.capacity_suppressed", warm_queue,
            "near-cap suppressed bundles must never be re-warmed")
    require(r"s_cand_overflow\s*\+=\s*\(uint64_t\)needed", shortfall,
            "suppressed manifest identities must remain visible in telemetry")

    load_one = function_body(source, "load_one_dll")
    parse_pos = load_one.find("parse_manifest")
    guard_pos = load_one.find("cache_entry_suppress_at_capacity")
    load_pos = load_one.find("load_overlay_dll")
    if (parse_pos < 0 or guard_pos < 0 or load_pos < 0 or
            parse_pos > guard_pos or guard_pos > load_pos):
        raise AssertionError(
            "capacity guard must parse aliases before platform DLL loading")
    require(r"manifest_has_pair_id\s*&&\s*loaded_pair_find", load_one,
            "known zero-slot pair aliases must bypass full-cap suppression")
    alias_pos = load_one.find("known_alias")
    shortfall_pos = load_one.find("cache_entry_suppress_for_shortfall")
    if alias_pos < 0 or shortfall_pos < 0 or alias_pos > shortfall_pos:
        raise AssertionError(
            "alias lookup must precede durable near/full-cap suppression")
    require(r"registered\s*==\s*0", load_one,
            "rejected loads must remain retryable")
    require(r"registered\s*>\s*0\)\s*s_ndlls\+\+", load_one,
            "pair aliases must not claim a DLL owner/flush slot")

    range_match = function_body(source, "range_candidate_matches")
    require(r"first_range_lo\s*=\s*UINT32_MAX", range_match,
            "CPS ownership must find the canonical lowest range root")
    require(r"lo\s*<\s*first_range_lo", range_match,
            "CPS ownership must not accept an alias at a later range island")
    require(r"!contains\s*\|\|\s*c->addr\s*!=\s*first_range_lo", range_match,
            "hosted interior aliases must fail closed for CPS range lookup")

    first_loader = source.index("static int load_overlay_dll")
    windows_start = source.rfind("#ifdef _WIN32", 0, first_loader)
    if windows_start < 0:
        raise AssertionError("missing Windows overlay-loader branch")
    posix_start = source.index("#else", windows_start)
    platform_end = source.index("#endif", posix_start)
    windows = source[windows_start:posix_start]
    posix = source[posix_start:platform_end]
    require(r"if\s*\(registered\s*==\s*0\)\s*\{[^}]*"
            r"s_dll_flush\[dll\]\s*=\s*NULL\s*;[^}]*FreeLibrary\(h\)", windows,
            "Windows zero-candidate loads must clear callbacks and release the DLL")
    win_preflight = windows.find("manifest/export mismatch")
    win_register = windows.find("cand_register")
    if win_preflight < 0 or win_register < 0 or win_preflight > win_register:
        raise AssertionError(
            "Windows must reject a partial-export manifest before registration")
    require(r"if\s*\(registered\s*==\s*0\)\s*\{[^}]*"
            r"s_dll_flush\[dll\]\s*=\s*NULL\s*;[^}]*"
            r"psx_overlay_posix_library_close\(h\)", posix,
            "POSIX zero-candidate loads must clear callbacks and release the DLL")
    posix_preflight = posix.find("manifest/export mismatch")
    posix_register = posix.find("cand_register")
    if (posix_preflight < 0 or posix_register < 0 or
            posix_preflight > posix_register):
        raise AssertionError(
            "POSIX must reject a partial-export manifest before registration")
    for platform, body, close in (
            ("Windows", windows, "FreeLibrary"),
            ("POSIX", posix, "psx_overlay_posix_library_close")):
        alias = body.find("loaded_pair_find")
        init = body.find("init_fn(&s_callbacks)")
        register_pos = body.find("cand_register")
        if alias < 0 or init < 0 or register_pos < 0 or not (
                alias < init < register_pos):
            raise AssertionError(
                f"{platform} aliases must close before init/registration")
        require(r"man_n\s*>\s*CAND_CAP\s*-\s*s_cand_n", body,
                f"{platform} new pairs must reject partial capacity atomically")
        require(r"registered\s*==\s*man_n\s*&&\s*manifest_has_pair_id\)\s*"
                r"loaded_pair_commit", body,
                f"{platform} must authorize aliases only after complete registration")
        require(rf"LOAD_PAIR_ALIAS[^;]*;", body,
                f"{platform} must return a distinct satisfied-alias result")
        require(rf"{close}\(h\)", body,
                f"{platform} alias validation must release its redundant handle")

    require(r'\\"candidate_overflow\\":%llu', debug,
            "debug status JSON must expose candidate overflow")
    require(r"overlay_loader_candidate_overflow\(\)", debug,
            "debug status must source candidate overflow from the loader getter")
    require(r'\\"pair_aliases\\":%llu', debug,
            "debug status JSON must expose validated whole-pair aliases")
    require(r"overlay_loader_pair_aliases\(\)", debug,
            "debug status must source pair aliases from the loader getter")

    print("PASS: overlay candidate capacity is bounded, visible, and fail-closed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, ValueError) as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
