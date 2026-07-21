#!/usr/bin/env python3
"""
compile_overlays.py — Offline overlay compilation primitive (B-2 / A-2 shared path).

Reads overlay_captures.json, compiles each code overlay to a DLL, and writes
it to the cache directory.  The runtime's A-1 LoadLibrary path reads from the
same cache.

Usage:
  python3 tools/compile_overlays.py \\
      --captures  build-dev/overlay_captures.json \\
      --game-toml game.toml \\
      --recompiler psxrecomp/recompiler/build/psxrecomp-game.exe \\
      --runtime-include psxrecomp/runtime/include \\
      --out-dir   build-dev/cache

Each DLL is written to <out-dir>/<game-id>/<crc32hex>.dll.
Each DLL exports:
  overlay_init(dispatch_fn)  — call once after LoadLibrary to wire dispatch
  func_XXXXXXXX(CPUState*)   — one export per compiled function entry point
"""

import argparse
import base64
import binascii
from bisect import bisect_left
from collections import Counter, deque
from contextlib import contextmanager, nullcontext
import glob
import hashlib
import heapq
import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid


try:
    import tomllib  # Python 3.11+
except ImportError:
    try:
        import tomli as tomllib
    except ImportError:
        print("ERROR: need tomllib (Python 3.11+) or 'pip install tomli'")
        sys.exit(1)

import json
import platform
import re


def codegen_ver(runtime_include: str) -> int:
    """Parse PSX_OVERLAY_CODEGEN_VER from overlay_api.h so the cache path version
    is the SAME value the runtime (overlay_loader.c) uses — the two can't drift.
    The cache is namespaced gcc/<arch-abi>/cg<N>/, so a build with new emitter
    output reads+writes a FRESH dir and never reuses a stale DLL."""
    hdr = os.path.join(runtime_include, 'overlay_api.h')
    with open(hdr) as f:
        m = re.search(r'#define\s+PSX_OVERLAY_CODEGEN_VER\s+(\d+)', f.read())
    if not m:
        raise SystemExit(f'PSX_OVERLAY_CODEGEN_VER not found in {hdr}')
    return int(m.group(1))


def overlay_abi_tag(runtime_include: str, flavor: int) -> int:
    """Return the exact ABI/flavor export expected from a cached shard."""
    hdr = os.path.join(runtime_include, 'overlay_api.h')
    with open(hdr) as source:
        match = re.search(
            r'#define\s+PSX_OVERLAY_ABI_VERSION\s+(\d+)', source.read())
    if not match:
        raise SystemExit(f'PSX_OVERLAY_ABI_VERSION not found in {hdr}')
    return (int(match.group(1)) & 0xFFFF) | ((int(flavor) & 0xFFFF) << 16)


def overlay_candidate_cap(runtime_include: str) -> int:
    """Return the loader's process-lifetime manifest-candidate capacity."""
    hdr = os.path.join(runtime_include, 'overlay_api.h')
    with open(hdr) as source:
        match = re.search(
            r'#define\s+PSX_OVERLAY_CANDIDATE_CAP\s+(\d+)', source.read())
    if not match:
        raise SystemExit(f'PSX_OVERLAY_CANDIDATE_CAP not found in {hdr}')
    value = int(match.group(1))
    if value <= 0:
        raise SystemExit(f'invalid PSX_OVERLAY_CANDIDATE_CAP in {hdr}: {value}')
    return value


def codegen_hash(runtime_include: str) -> int:
    """Parse PSX_OVERLAY_CODEGEN_HASH from the build-generated overlay_codegen_hash.h
    (next to overlay_api.h). Folded into the cache path as cg<N>_<hash> so ANY
    emitter change auto-invalidates the cache (no stale-but-cgN reuse). Falls back
    to 0 when the header is absent (a tree not yet built) — matching overlay_api.h's
    __has_include fallback, so the loader and compiler still agree on the path."""
    hdr = os.path.join(runtime_include, 'overlay_codegen_hash.h')
    try:
        with open(hdr) as f:
            m = re.search(r'#define\s+PSX_OVERLAY_CODEGEN_HASH\s+0x([0-9A-Fa-f]+)', f.read())
        if m:
            return int(m.group(1), 16)
    except FileNotFoundError:
        pass
    return 0


def verify_recompiler_matches_tag(recompiler: str, tag_hash: int) -> None:
    """Stale-recompiler-binary guard. The cg tag hash is computed from the
    EMITTER SOURCES (via --runtime-include's overlay_codegen_hash.h), but the
    code is emitted by the --recompiler BINARY — nothing else ties the two.
    A recompiler built before the last emitter change happily emits OLD code
    that gets stamped with the CURRENT tag: read tag == write tag, content
    stale (the 2026-07-01 Tomba pause-menu wedge; loaded-save scene shards
    corrupted the display-list queue while every tag check passed).

    psxrecomp-game bakes the same hash at ITS build time and prints it via
    --codegen-hash. Any mismatch — including a binary too old to support the
    flag — is a HARD failure: rebuild the recompiler, never build shards."""
    import subprocess
    try:
        out = subprocess.run([recompiler, '--codegen-hash'],
                             capture_output=True, text=True, timeout=30)
    except Exception as e:
        raise SystemExit(f'FATAL: cannot execute {recompiler} for --codegen-hash '
                         f'staleness check: {e}')
    line = (out.stdout or '').strip().splitlines()
    baked = line[0].strip() if line else ''
    if out.returncode != 0 or not re.fullmatch(r'[0-9a-fA-F]{8}', baked or ''):
        raise SystemExit(
            f'FATAL: {recompiler} does not support --codegen-hash (or errored).\n'
            f'  This binary predates the stale-recompiler guard and CANNOT be\n'
            f'  verified against the emitter sources. Rebuild it:\n'
            f'    cmake --build <recompiler-build-dir> --target psxrecomp-game')
    if int(baked, 16) != tag_hash:
        raise SystemExit(
            f'FATAL: STALE RECOMPILER BINARY.\n'
            f'  {recompiler}\n'
            f'  was built from emitter sources hashing {baked}, but the runtime\n'
            f'  tree stamps cache tag hash {tag_hash:08x}. Shards it emits would\n'
            f'  carry the current tag with OLD codegen semantics (the silent\n'
            f'  stale-shard class). Rebuild the recompiler first:\n'
            f'    cmake --build <recompiler-build-dir> --target psxrecomp-game')
    print(f'recompiler codegen hash verified: {baked} == cg tag hash')


def is_windows() -> bool:
    """True on native Windows AND under MSYS/Cygwin/MinGW pythons.
    platform.system() there returns 'MSYS_NT-...'/'CYGWIN_NT-...', NOT
    'Windows' — the naive check filed a whole session's overlay DLLs under
    gcc/linux-x64/ while the Windows loader read gcc/win-x64/: the runtime
    interpreted 'covered' functions forever (Tomba2 attract ran ~half its
    instruction volume on the interpreter, 2026-07-02) while autocompile
    kept reporting 'already covered - no new native code to build'."""
    return (os.name == 'nt'
            or platform.system() == 'Windows'
            or platform.system().startswith(('MSYS', 'CYGWIN', 'MINGW')))


def overlay_ext() -> str:
    """Use the host platform's conventional shared-library suffix."""
    return '.dll' if is_windows() else '.so'


def native_path(p: str) -> str:
    """Absolute form of `p`, in the RUNNING interpreter's own path flavor, for
    handing to native tools (gcc, tcc). Two hard-won rules live here
    (2026-07-15, runtime `cmd.exe /C python` resolved to devkitPro's MSYS
    python and every in-game shard compile died exit-1/empty-stderr):

    1. ABSOLUTE is mandatory: gcc invoked from cmd.exe silently fails on
       relative forward-slash paths.
    2. Do NOT translate flavors here. Under an MSYS-flavored python, POSIX
       args (/f/..., /home/...) are translated to Windows form AT THE EXEC
       BOUNDARY by the msys runtime using its OWN mount table — verified
       correct (gcc -v showed /home/... arriving as C:/Users/...). Translating
       here with `cygpath`/heuristics uses whatever foreign cygpath is first
       on PATH (e.g. Git-for-Windows') and corrupts the path. The actual
       silent-failure root cause was never the args — it was the child PATH
       flavor (_toolchain_env below)."""
    return os.path.abspath(p)


def cache_arch_abi() -> str:
    """Canonical cache arch-abi tag, IDENTICAL to overlay_loader.c's
    PSX_OVERLAY_ARCH_ABI ("<os>-<arch>": win|linux|macos + x64|arm64|x86).
    gcc DLLs are namespaced under <game_id>/gcc/<arch-abi>/ so same-OS
    different-arch caches never comingle. Keep this
    mapping in lockstep with overlay_loader.c."""
    if is_windows():
        os_tag = 'win'
    else:
        os_tag = {'Darwin': 'macos'}.get(platform.system(), 'linux')
    m = platform.machine().lower()
    if m in ('amd64', 'x86_64', 'x64'):
        arch = 'x64'
    elif m in ('arm64', 'aarch64'):
        arch = 'arm64'
    elif m in ('i386', 'i686', 'x86'):
        arch = 'x86'
    else:
        arch = 'unknown'
    return f'{os_tag}-{arch}'


# ---------------------------------------------------------------------------
# Shard build accounting — make failures LOUD
# ---------------------------------------------------------------------------
# Historically a per-shard failure (recompiler error, generated-C audit reject,
# gcc/tcc compile error, dropped interior fragment) only printed a line into an
# ephemeral log and the script still exited 0. The runtime's autocompile watcher
# keys "did it work?" off the exit code, so a header change that broke EVERY
# shard looked identical to a fully-successful run: the affected code just ran
# interpreted forever. ShardStats fixes that — every build outcome is tallied
# (thread-safe, since captures compile on a pool), a SUMMARY prints at the end,
# a machine-readable PSX_SHARD_RESULT line is emitted for the runtime to parse,
# and main() exits non-zero when any shard that SHOULD have built failed.
class ShardStats:
    def __init__(self):
        self._lock = threading.Lock()
        self.ok = 0
        self.skipped = 0
        self.capacity_fastpath = 0
        self.fail_by_class = Counter()
        self.failures = []          # [(label, cls, detail), ...] (detail truncated)

    def add_ok(self, n=1):
        with self._lock:
            self.ok += n

    def add_skip(self, n=1):
        with self._lock:
            self.skipped += n

    def add_capacity_fastpath(self, n=1):
        with self._lock:
            self.capacity_fastpath += n

    def add_fail(self, label, cls, detail=''):
        # Print immediately AND record, so the failure is visible in the live
        # stream (per-worker buffered) and in the end-of-run summary.
        d = (detail or '').strip().replace('\n', ' ')
        if len(d) > 240:
            d = d[:237] + '...'
        print(f'  SHARD FAIL [{cls}] {label}{(": " + d) if d else ""}')
        with self._lock:
            self.fail_by_class[cls] += 1
            self.failures.append((label, cls, d))

    def total_fail(self):
        with self._lock:
            return sum(self.fail_by_class.values())

    def print_summary(self):
        with self._lock:
            fail = sum(self.fail_by_class.values())
            print('\n=== SHARD BUILD SUMMARY ===')
            print(f'  built OK : {self.ok}')
            print(f'  skipped  : {self.skipped}  '
                  '(cached / data-only / safe coverage loss)')
            if self.capacity_fastpath:
                print(f'  cap-fast : {self.capacity_fastpath} cache recipe '
                      'job(s) skipped at an already-full fixed point')
            print(f'  FAILED   : {fail}')
            for cls, n in sorted(self.fail_by_class.items()):
                print(f'    - {cls}: {n}')
            for label, cls, detail in self.failures[:40]:
                print(f'    ! [{cls}] {label}{(": " + detail) if detail else ""}')
            if len(self.failures) > 40:
                print(f'    ... {len(self.failures) - 40} more')
            # Stable, grep-able result line the runtime (autocompile.c) parses to
            # surface shard_ok / shard_fail without depending on the exit code.
            print(f'PSX_SHARD_RESULT ok={self.ok} failed={fail} '
                  f'skipped={self.skipped} '
                  f'capacity_fastpath={self.capacity_fastpath}')
        return fail


# ---------------------------------------------------------------------------
# PS-EXE fake header
# ---------------------------------------------------------------------------

def make_psxexe(load_addr: int, entry_pc: int, data: bytes) -> bytes:
    """Wrap raw overlay bytes in a minimal PS-EXE header."""
    header = bytearray(2048)
    header[0:8]   = b'PS-X EXE'
    struct.pack_into('<I', header, 0x10, entry_pc)   # initial PC
    struct.pack_into('<I', header, 0x14, 0)           # initial GP
    struct.pack_into('<I', header, 0x18, load_addr)   # load address
    struct.pack_into('<I', header, 0x1C, len(data))   # text size
    return bytes(header) + data


# ---------------------------------------------------------------------------
# Overlay seed and generated-C audits
# ---------------------------------------------------------------------------

INCLUDE_REASONS = {
    'DISPATCH_ENTRY',
    # Export/header/jump-table entry proven directly by a static extractor.
    # It passes the same callable-boundary gate as a generic dispatch entry,
    # but retains provenance that may nominate the address in sibling bytes.
    'STATIC_DISPATCH_ENTRY',
    'DIRECT_JAL_TARGET',
    'STATIC_INDIRECT_TARGET',
    'STATIC_BRANCH_ROOT',
    # Play-free normal-mode discovery entry that also passes a target-local
    # callable-boundary/CFG proof. Stronger than a generic captured pointer.
    'STATIC_DISCOVERY_ROOT',
    'FUNCTION_POINTER_TARGET',
    'TOML_DECLARED_ENTRY',
    # Dispatch-proven PC with no callable boundary (mid-function code reached
    # through a function pointer or dispatch chain). Compiled as an
    # overlapping-alias entry into its host function — written to the seeds
    # file as 'interior 0x...' so the recompiler never uses it as a walk root
    # (a walk root there would truncate the host: the mid-function-seed
    # softlock class).
    'DISPATCH_INTERIOR',
    # Kernel-window-only promotion of an ORPHAN dispatch interior: a
    # dispatch-proven PC in kernel RAM [0, 0x10000) that no rooted walk
    # covers. There is no host to alias into and no host a root could
    # truncate — the static recompiler's install-slot hooks tail-dispatch
    # into exactly such PCs (e.g. RAM 0xCF0, the SIO data-byte stub).
    # Written to the seeds file as 'dispatch_root 0x...': a trusted walk
    # root, exempt from the recompiler's boundary re-check. Overlay regions
    # are NOT eligible: per-PC dispatch evidence persists across scene
    # variants there, so an orphan may belong to a non-resident variant
    # whose bytes in THIS image are data (the 0xE889C class) — rooting it
    # would walk garbage.
    'DISPATCH_ROOT',
}
EXACT_FRAGMENT_REASONS = INCLUDE_REASONS - {'DISPATCH_INTERIOR'}
# Cross-variant propagation uses only explicit static code-flow evidence or a
# statically extracted export/header dispatch entry. Runtime observations,
# generic pointer scans, TOML entries, and promoted dispatch roots are useful
# local evidence, but are not durable sibling-variant nominations.
CROSS_VARIANT_DONOR_REASONS = {
    'DIRECT_JAL_TARGET',
    'STATIC_INDIRECT_TARGET',
    'STATIC_BRANCH_ROOT',
    'STATIC_DISCOVERY_ROOT',
    'STATIC_DISPATCH_ENTRY',
}
# A recipient host is not inferred: it must already be a runtime-valid exact F
# entry for these bytes and retain one of the classifier's local root reasons.
HOSTED_OWNER_REASONS = EXACT_FRAGMENT_REASONS - {'DISPATCH_ROOT'}
FATAL_SEED_REASONS = {'BRANCH_TARGET_ONLY', 'OBSERVED_PC_ONLY', 'UNKNOWN'}
BIOS_RESIDENT_PRODUCER = 'bios_resident_manifest'
BIOS_RESIDENT_MARKER = 'psxrecomp bios resident shard v1'

# Hosted aliases are supplemental optimizations, never correctness authority.
# Keep every phase deterministically bounded so adversarial/malformed capture
# sets cannot turn one compile invocation into unbounded shard fan-out.
HOSTED_NOMINATION_CAP = 4096
# Python selection precedes the C++ organic-block proof, so most candidates are
# expected to be rejected without publication. The emitted-identity cap below
# remains the tighter runtime-facing limit.
HOSTED_SELECTED_ALIAS_CAP = 4096
HOSTED_SELECTED_HOST_CAP = 512
HOSTED_BATCH_HOST_CAP = 64
HOSTED_BATCH_ALIAS_CAP = 256
# A terminal partition/partial-success tree over N targets has at most 2N-1
# nodes. Budget the entire selected universe so a second invocation can never
# resume an attempt-capped suffix.
HOSTED_COMPILE_ATTEMPT_CAP = 2 * HOSTED_SELECTED_ALIAS_CAP - 1
HOSTED_EMITTED_IDENTITY_CAP = 1024
MANIFEST_LINE_MAX = 128
MANIFEST_PHYSICAL_LINE_MAX = 159
MANIFEST_ASCII_WHITESPACE = ' \t\r\v\f'
HOSTED_MANIFEST_PROVENANCE = 'hosted-v1'
ORPHAN_MANIFEST_PROVENANCE = 'orphan-v1'
NON_AUTHORITY_MANIFEST_PROVENANCES = {
    HOSTED_MANIFEST_PROVENANCE, ORPHAN_MANIFEST_PROVENANCE,
}
HOSTED_MANIFEST_MARKER = (
    f'# psxrecomp overlay provenance {HOSTED_MANIFEST_PROVENANCE}')
HOSTED_UNIQUE_GUARDED_BYTE_CAP = 1024 * 1024
PSX_RAM_SIZE = 2 * 1024 * 1024


class ShardCandidateCapacityError(RuntimeError):
    """A cache publication would exceed the runtime's fixed F-record table."""


def manifest_has_hosted_provenance(manifest: str) -> bool:
    """Recognize the exact compiler-owned marker under LF or CRLF records."""
    return manifest_provenance(manifest) == HOSTED_MANIFEST_PROVENANCE


def manifest_declares_supplemental_provenance(manifest: str) -> bool:
    """Fail closed for any compiler provenance record, known or malformed."""
    prefix = '# psxrecomp overlay provenance '
    return any(
        (raw_line[:-1] if raw_line.endswith('\r') else raw_line).startswith(
            prefix)
        for raw_line in manifest.split('\n')
    )


def manifest_provenance(manifest: str) -> str | None:
    """Return one exact compiler-owned non-authority provenance record."""
    prefix = '# psxrecomp overlay provenance '
    found = []
    for raw_line in manifest.split('\n'):
        line = raw_line[:-1] if raw_line.endswith('\r') else raw_line
        if line.startswith(prefix):
            found.append(line[len(prefix):])
    if len(found) != 1 or found[0] not in NON_AUTHORITY_MANIFEST_PROVENANCES:
        return None
    return found[0]


def seed_line_for_reason(addr: int, reason: str) -> str:
    """Serialize one classifier reason without weakening its root contract."""
    if reason == 'DISPATCH_INTERIOR':
        return f'interior 0x{addr:08X}'
    if reason == 'DISPATCH_ROOT':
        return f'dispatch_root 0x{addr:08X}'
    if reason in ('DIRECT_JAL_TARGET', 'STATIC_INDIRECT_TARGET',
                  'STATIC_BRANCH_ROOT', 'STATIC_DISCOVERY_ROOT'):
        return f'call_root 0x{addr:08X}'
    return f'0x{addr:08X}'


def update_bios_resident_marker(dll_path: str, cap: dict) -> None:
    """Publish/remove the opt-in marker consumed by the runtime preloader.

    Resident BIOS helpers use a synthetic page envelope, so their filename's
    region key cannot equal the dirty-run start recovered at runtime.  The
    marker tells the loader to register the shard at cache scan time; normal
    per-function code-CRC validation remains the execution authority.
    """
    marker = os.path.splitext(dll_path)[0] + '.resident'
    if cap.get('producer') != BIOS_RESIDENT_PRODUCER:
        try:
            os.remove(marker)
        except FileNotFoundError:
            pass
        return
    payload = {
        'schema': BIOS_RESIDENT_MARKER,
        'bios_sha256': str(cap.get('bios_sha256', '')),
        'producer_name': str(cap.get('producer_name', 'BIOS resident code')),
    }
    serialized = json.dumps(payload, sort_keys=True) + '\n'
    try:
        with open(marker, 'r', encoding='utf-8') as f:
            if f.read(len(serialized) + 1) == serialized:
                return
    except (FileNotFoundError, OSError, UnicodeError):
        pass
    tmp = f'{marker}.tmp.{uuid.uuid4().hex}'
    try:
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write(serialized)
        os.replace(tmp, marker)
    finally:
        _best_effort_unlink(tmp)


def reconcile_bios_resident_marker(dll_path: str, cap: dict,
                                   published: bool) -> None:
    """Keep the winner's residency, while letting resident evidence self-heal.

    A losing ordinary writer must not remove a resident winner's sidecar. A
    resident capture may always ensure the marker: identical region bytes make
    the preserved shard equally safe to preload, and this repairs a crash
    between pair publication and sidecar publication.
    """
    # Once this exact canonical region identity has verified resident evidence,
    # retaining the marker is safe even if an ordinary capture won the compile
    # race: every function still passes the live per-range CRC guard. Never let
    # an ordinary writer race a resident writer by removing that evidence.
    if cap.get('producer') == BIOS_RESIDENT_PRODUCER:
        update_bios_resident_marker(dll_path, cap)


def _parse_addr(value) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 16)


def _parse_addr_list(values) -> set[int]:
    out = set()
    for v in values or []:
        try:
            out.add(_parse_addr(v))
        except (TypeError, ValueError):
            pass
    return out


def optional_enrichment_fallback_capture(cap: dict) -> dict | None:
    """Return a conservative retry recipe declared by a static extractor.

    The richer recipe is always attempted first.  If its generated C or host
    compile is rejected, these independently proven roots preserve the safe
    shard while all optional aliases and normal-mode discoveries are dropped.
    """
    key = 'optional_enrichment_fallback_entry_pcs'
    recipe = cap.get(key, [])
    fallback = dict(cap)
    if isinstance(recipe, dict):
        functions = sorted(_parse_addr_list(
            recipe.get('function_entry_pcs', [])))
        dispatch = sorted(_parse_addr_list(
            recipe.get('dispatch_entry_pcs', [])))
        static = sorted(_parse_addr_list(
            recipe.get('static_discovery_entry_pcs', [])))
        static_dispatch = sorted(_parse_addr_list(
            recipe.get('static_dispatch_entry_pcs', [])))
        if not (functions or dispatch or static):
            return None
        fallback['function_entry_pcs'] = [
            f'0x{entry:08X}' for entry in functions]
        fallback['dispatch_entry_pcs'] = [
            f'0x{entry:08X}' for entry in dispatch]
        fallback['static_dispatch_entry_pcs'] = [
            f'0x{entry:08X}' for entry in static_dispatch
            if entry in set(dispatch)]
        fallback['static_discovery_entry_pcs'] = [
            f'0x{entry:08X}' for entry in static]
        fallback['seeds'] = fallback['function_entry_pcs']
        if 'producer_ranges' in recipe:
            fallback['producer_ranges'] = recipe['producer_ranges']
        if 'strict_producer_ranges' in recipe:
            fallback['strict_producer_ranges'] = bool(
                recipe['strict_producer_ranges'])
    else:
        entries = sorted(_parse_addr_list(recipe))
        if not entries:
            return None
        encoded = [f'0x{entry:08X}' for entry in entries]
        fallback['function_entry_pcs'] = encoded
        fallback['dispatch_entry_pcs'] = encoded
        fallback['static_dispatch_entry_pcs'] = []
        fallback['static_discovery_entry_pcs'] = encoded
        fallback['seeds'] = encoded
    fallback.pop('static_alias_ranges', None)
    fallback.pop('static_jump_table_proofs', None)
    fallback.pop('static_call_continuation_pcs', None)
    fallback.pop('_prior_aliases', None)
    fallback.pop(key, None)
    return fallback


def _word_at(data: bytes, load_addr: int, addr: int):
    off = addr - load_addr
    if off < 0 or off + 4 > len(data):
        return None
    return struct.unpack_from('<I', data, off)[0]


def _is_jr_ra(word) -> bool:
    return word == 0x03E00008


def _is_addiu_sp_neg(word) -> bool:
    if word is None:
        return False
    return ((word >> 26) & 0x3F) == 0x09 \
        and ((word >> 21) & 0x1F) == 29 \
        and ((word >> 16) & 0x1F) == 29 \
        and (word & 0x8000) != 0


def _is_control_flow(word) -> bool:
    if word is None:
        return False
    op = (word >> 26) & 0x3F
    fn = word & 0x3F
    return (op in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                   0x14, 0x15, 0x16, 0x17) or
            (op == 0 and fn in (0x08, 0x09)) or
            (op in (0x11, 0x12) and ((word >> 21) & 0x1F) == 0x08))


def _is_valid_mips_word(word) -> bool:
    if word is None or word in (0xFFFFFFFF, 0xFFFFFFFD):
        return False

    op = (word >> 26) & 0x3F
    fn = word & 0x3F
    rt = (word >> 16) & 0x1F

    if op == 0x00:
        return fn in {
            0x00, 0x02, 0x03, 0x04, 0x06, 0x07,
            0x08, 0x09, 0x0C, 0x0D,
            0x10, 0x11, 0x12, 0x13,
            0x18, 0x19, 0x1A, 0x1B,
            0x20, 0x21, 0x22, 0x23,
            0x24, 0x25, 0x26, 0x27,
            0x2A, 0x2B,
        }
    if op == 0x01:
        return rt in (0x00, 0x01, 0x10, 0x11)

    return op in {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x12,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x28, 0x29, 0x2A, 0x2B, 0x2E,
        0x30, 0x32, 0x38, 0x3A,
    }


def _jump_target(pc: int, word: int) -> int:
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def _branch_target(pc: int, word: int) -> int:
    imm = word & 0xFFFF
    if imm & 0x8000:
        imm -= 0x10000
    return pc + 4 + (imm << 2)


def _classify_cf(pc: int, word: int) -> tuple[str, int]:
    op = (word >> 26) & 0x3F
    fn = word & 0x3F
    rs = (word >> 21) & 0x1F
    if op == 0 and fn == 0x08:
        return ('jr_ra' if rs == 31 else 'jr', 0)
    if op == 0 and fn == 0x09:
        return ('jalr', 0)
    if op == 0x02:
        return ('j', _jump_target(pc, word))
    if op == 0x03:
        return ('jal', _jump_target(pc, word))
    rt = (word >> 16) & 0x1F
    if op in (0x04, 0x14) and rs == rt:
        return ('j', _branch_target(pc, word))
    if op == 0x05 and rs == rt:
        return ('branch_never', 0)
    if op == 0x15 and rs == rt:
        return ('branch_never_likely', 0)
    if op in (0x06, 0x16) and rs == 0:
        return ('j', _branch_target(pc, word))
    if op == 0x07 and rs == 0:
        return ('branch_never', 0)
    if op == 0x17 and rs == 0:
        return ('branch_never_likely', 0)
    if op == 0x01 and rs == 0:
        if rt in (0x00, 0x10):       # bltz / bltzal
            return ('branch_never', 0)
        if rt in (0x02, 0x12):       # bltzl / bltzall
            return ('branch_never_likely', 0)
        if rt in (0x01, 0x03):       # bgez / bgezl
            return ('j', _branch_target(pc, word))
        if rt in (0x11, 0x13):       # bal / ball aliases: target + continuation
            return ('jal', _branch_target(pc, word))
    if op in (0x11, 0x12) and rs == 0x08:
        return ('branch', _branch_target(pc, word))
    if op in (0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
        return ('branch', _branch_target(pc, word))
    return ('normal', 0)


def _find_jump_table_targets(data: bytes, load_addr: int, size: int,
                             entry: int, hard_cap: int,
                             jr_pc: int, jr_rs: int,
                             producer_range=None, proof_out=None) -> set[int]:
    """Resolve one tightly bounded canonical ``sltiu``/``jr`` switch.

    This is deliberately a structural reaching-definition proof, not the old
    40-word bag-of-opcodes backscan.  The accepted suffix is::

        sltiu guard,index,count; beq guard,zero,reject; nop
        sll scaled,index,2; addu address,scaled,table_base
        lw target,offset(address); [nop x 0..1]; jr target

    ``table_base`` must be a nearest-definition LUI, optionally followed by a
    nearest-definition ADDIU.  The lower instruction may rename the
    register (``lui v0; addiu s0,v0,lo``), as used by Ape Escape.  Any other
    control flow between that constant and the suffix, an inbound edge that
    skips the definition, a register mismatch/clobber, or one non-local table
    entry rejects the entire candidate.
    """
    lo = load_addr
    hi = load_addr + size
    if not (entry <= jr_pc < hard_cap and 0 < jr_rs < 32):
        return set()
    jr_word = _word_at(data, load_addr, jr_pc)
    if jr_word != ((jr_rs << 21) | 0x08):
        return set()

    # Permit only the compiler's zero-or-one scheduling NOP between LW/JR.
    lw_pc = jr_pc - 4
    for _ in range(1):
        if _word_at(data, load_addr, lw_pc) != 0:
            break
        lw_pc -= 4
    lw_word = _word_at(data, load_addr, lw_pc)
    if lw_word is None or ((lw_word >> 26) & 0x3F) != 0x23 \
            or ((lw_word >> 16) & 0x1F) != jr_rs:
        return set()
    address_reg = (lw_word >> 21) & 0x1F
    if not address_reg:
        return set()
    lw_offset = lw_word & 0xFFFF
    if lw_offset & 0x8000:
        lw_offset -= 0x10000

    addu_pc = lw_pc - 4
    addu_word = _word_at(data, load_addr, addu_pc)
    if addu_word is None or ((addu_word >> 26) & 0x3F) != 0 \
            or (addu_word & 0x3F) != 0x21 \
            or ((addu_word >> 6) & 0x1F) != 0 \
            or ((addu_word >> 11) & 0x1F) != address_reg:
        return set()
    addu_inputs = ((addu_word >> 21) & 0x1F,
                   (addu_word >> 16) & 0x1F)

    sll_pc = addu_pc - 4
    sll_word = _word_at(data, load_addr, sll_pc)
    if sll_word is None or ((sll_word >> 26) & 0x3F) != 0 \
            or (sll_word & 0x3F) != 0 \
            or ((sll_word >> 21) & 0x1F) != 0 \
            or ((sll_word >> 6) & 0x1F) != 2:
        return set()
    scaled_reg = (sll_word >> 11) & 0x1F
    index_reg = (sll_word >> 16) & 0x1F
    if not scaled_reg or not index_reg or scaled_reg not in addu_inputs:
        return set()
    table_reg = addu_inputs[1] if addu_inputs[0] == scaled_reg else addu_inputs[0]
    if not table_reg or table_reg == scaled_reg:
        return set()

    # The bound must guard this exact index on the sole fallthrough into SLL.
    sltiu_pc = sll_pc - 12
    branch_pc = sll_pc - 8
    if _word_at(data, load_addr, sll_pc - 4) != 0:
        return set()
    sltiu_word = _word_at(data, load_addr, sltiu_pc)
    branch_word = _word_at(data, load_addr, branch_pc)
    if sltiu_word is None or ((sltiu_word >> 26) & 0x3F) != 0x0B \
            or ((sltiu_word >> 21) & 0x1F) != index_reg:
        return set()
    guard_reg = (sltiu_word >> 16) & 0x1F
    table_count = sltiu_word & 0xFFFF
    if not guard_reg or not (1 <= table_count < 512):
        return set()
    if branch_word is None or ((branch_word >> 26) & 0x3F) != 0x04:
        return set()
    branch_rs = (branch_word >> 21) & 0x1F
    branch_rt = (branch_word >> 16) & 0x1F
    if {branch_rs, branch_rt} != {0, guard_reg}:
        return set()
    reject_target = _branch_target(branch_pc, branch_word)
    if (not (entry <= reject_target < hard_cap) or
            sll_pc <= reject_target < jr_pc + 8):
        return set()

    def nearest_writer(reg: int, before: int):
        for pc in range(before - 4, max(entry, before - 0x80) - 1, -4):
            word = _word_at(data, load_addr, pc)
            if word is None:
                return None
            if _instruction_writes_gpr(word, reg):
                return pc, word
        return None

    table_writer = nearest_writer(table_reg, addu_pc)
    if table_writer is None:
        return set()
    table_def_pc, table_def_word = table_writer
    def_op = (table_def_word >> 26) & 0x3F
    def_rs = (table_def_word >> 21) & 0x1F
    def_rt = (table_def_word >> 16) & 0x1F
    if def_op == 0x0F and def_rs == 0 and def_rt == table_reg:
        constant_pc = table_def_pc
        table_base = (table_def_word & 0xFFFF) << 16
    elif def_op == 0x09 and def_rt == table_reg and def_rs:
        upper_writer = nearest_writer(def_rs, table_def_pc)
        if upper_writer is None:
            return set()
        constant_pc, upper_word = upper_writer
        if ((upper_word >> 26) & 0x3F) != 0x0F \
                or ((upper_word >> 21) & 0x1F) != 0 \
                or ((upper_word >> 16) & 0x1F) != def_rs:
            return set()
        low = table_def_word & 0xFFFF
        upper = (upper_word & 0xFFFF) << 16
        if low & 0x8000:
            low -= 0x10000
        table_base = (upper + low) & 0xFFFFFFFF
    else:
        return set()

    # Calls could clobber the proven constant.  Other branches could enter the
    # suffix through an unproved path.  The one exact bounds branch is allowed.
    for pc in range(constant_pc + 4, sll_pc, 4):
        word = _word_at(data, load_addr, pc)
        if word is None:
            return set()
        if _is_control_flow(word) and pc != branch_pc:
            return set()
    predecessor = _word_at(data, load_addr, constant_pc - 4)
    if constant_pc >= entry + 4 and _is_control_flow(predecessor):
        return set()
    table_base = (table_base + lw_offset) & 0xFFFFFFFF
    table_end = table_base + table_count * 4
    range_lo, range_hi = producer_range or (lo, hi)
    if (table_base & 3) or not (range_lo <= table_base < table_end <= range_hi):
        return set()

    targets = set()
    for index in range(table_count):
        target = _word_at(data, load_addr, table_base + index * 4)
        if (target is None or (target & 3) or
                not (entry <= target < hard_cap) or
                not (range_lo <= target < range_hi) or
                not _is_valid_mips_word(_word_at(data, load_addr, target))):
            return set()
        targets.add(target)

    # Establish which later sources are themselves reached only after the
    # proven table dispatch. Such case blocks may safely loop to the suffix
    # (Ape does this); an arbitrary earlier/later byte sequence may not jump
    # into the protected definitions and manufacture domination.
    case_reachable = set()
    work = deque(targets)
    while work and len(case_reachable) < 2048:
        pc = work.popleft()
        if (pc in case_reachable or not (entry <= pc < hard_cap) or
                not (range_lo <= pc < range_hi)):
            continue
        word = _word_at(data, load_addr, pc)
        if not _is_valid_mips_word(word):
            continue
        case_reachable.add(pc)
        kind, target = _classify_cf(pc, word)
        delay = pc + 4
        if kind == 'normal':
            work.append(pc + 4)
        elif kind == 'branch':
            case_reachable.add(delay)
            work.extend((pc + 8, target))
        elif kind == 'branch_never':
            case_reachable.add(delay)
            work.append(pc + 8)
        elif kind == 'branch_never_likely':
            work.append(pc + 8)
        elif kind == 'j':
            case_reachable.add(delay)
            work.append(target)
        elif kind in ('jal', 'jalr'):
            case_reachable.add(delay)
            work.append(pc + 8)
        elif kind in ('jr', 'jr_ra'):
            case_reachable.add(delay)

    for source in range(entry, hard_cap, 4):
        source_word = _word_at(data, load_addr, source)
        if source_word is None:
            return set()
        kind, target = _classify_cf(source, source_word)
        if (kind in ('branch', 'j', 'jal') and
                constant_pc < target <= jr_pc and
                not (constant_pc <= source <= jr_pc) and
                source not in case_reachable):
            return set()
    if proof_out is not None:
        proof_out.append({
            'host_entry': entry,
            'jr_pc': jr_pc,
            'table_base': table_base,
            'count': table_count,
            'targets': sorted(targets),
        })
    return targets


def _callable_legacy_seed(data: bytes, load_addr: int, addr: int) -> bool:
    """Conservative fallback for old captures that only had a `seeds` list."""
    word = _word_at(data, load_addr, addr)
    prev = _word_at(data, load_addr, addr - 4)
    prev8 = _word_at(data, load_addr, addr - 8)
    if not _is_valid_mips_word(word):
        return False
    if addr == load_addr:
        return True
    if _is_jr_ra(prev8):
        return True
    return _is_addiu_sp_neg(word) and not _is_control_flow(prev)


def _dense_local_pointer_table(data: bytes, load_addr: int,
                               addr: int, count: int = 3) -> bool:
    """Reject instruction-shaped data at a supposed callable target."""
    hi = load_addr + len(data)
    for index in range(count):
        value = _word_at(data, load_addr, addr + index * 4)
        if value is None or (value & 3) or not (load_addr <= value < hi):
            return False
    return True


def _callable_direct_jal_target(data: bytes, load_addr: int, addr: int) -> bool:
    """True only when a direct JAL target has independent boundary evidence."""
    return (not _dense_local_pointer_table(data, load_addr, addr) and
            _callable_legacy_seed(data, load_addr, addr))


def _instruction_writes_gpr(word: int, reg: int) -> bool:
    if not reg:
        return False
    op = (word >> 26) & 0x3F
    rs = (word >> 21) & 0x1F
    rt = (word >> 16) & 0x1F
    rd = (word >> 11) & 0x1F
    fn = word & 0x3F
    if op == 0:
        if fn in (0x08, 0x0C, 0x0D, 0x11, 0x13,
                  0x18, 0x19, 0x1A, 0x1B):
            return False
        return rd == reg
    if op == 0x03:
        return reg == 31
    if op == 0x01 and rt in (0x10, 0x11):
        return reg == 31
    if op in range(0x08, 0x10) or op in range(0x20, 0x27) \
            or op in (0x30, 0x32):
        return rt == reg
    if op in (0x10, 0x12) and rs in (0, 2):
        return rt == reg
    return False


def _resolve_constant_transfer(data: bytes, load_addr: int,
                               entry: int, transfer_pc: int,
                               target_reg: int):
    """Resolve a same-basic-block lui + addiu/ori constant feeding jr/jalr."""
    low = None
    low_is_addiu = False
    for count in range(1, 17):
        pc = transfer_pc - count * 4
        if pc < entry:
            break
        word = _word_at(data, load_addr, pc)
        if word is None:
            break
        op = (word >> 26) & 0x3F
        rs = (word >> 21) & 0x1F
        rt = (word >> 16) & 0x1F
        if low is None and op in (0x09, 0x0D) \
                and rs == target_reg and rt == target_reg:
            low = word & 0xFFFF
            low_is_addiu = op == 0x09
            continue
        if op == 0x0F and rt == target_reg:
            # A LUI in a control-transfer delay slot is not a local reaching
            # definition for the following suffix. A callee may clobber the
            # register before returning at LUI+4, while a branch/jump can leave
            # the linear path entirely. Fail closed without path/call proof.
            predecessor = _word_at(data, load_addr, pc - 4)
            if pc >= entry + 4 and _is_control_flow(predecessor):
                return None

            # Reject a syntactic constant pair when a direct control-flow edge
            # can enter after this LUI.  Example: `j low; lui; low: addiu;
            # jalr` never executes the LUI, despite looking like a pair to a
            # raw backward scan.
            crossed_inbound_boundary = False
            for source in range(entry, transfer_pc, 4):
                source_word = _word_at(data, load_addr, source)
                if source_word is None:
                    continue
                source_kind, source_target = _classify_cf(source, source_word)
                if (source_kind in ('branch', 'j', 'jal') and
                        pc < source_target <= transfer_pc):
                    crossed_inbound_boundary = True
                    break
            if crossed_inbound_boundary:
                return None

            upper = (word & 0xFFFF) << 16
            if low is None:
                return upper
            if low_is_addiu and low & 0x8000:
                low -= 0x10000
            return (upper + low) & 0xFFFFFFFF if low_is_addiu else upper | low
        if _instruction_writes_gpr(word, target_reg) or _is_control_flow(word):
            break
    return None


def plausible_callable_target(data: bytes, load_addr: int, size: int,
                              target: int, producer_hi: int) -> bool:
    """CFG proof for a cross-producer call target without a classic prologue.

    Direct calls are sufficient boundary evidence inside one linked producer.
    Across a synthetic adjacent-producer envelope, additionally require a
    bounded, valid CFG with a reachable return and no sequential fallthrough.
    This retains real frameless exports while rejecting pointer tables/NOP
    runways that only happen to decode as instructions.
    """
    first = _word_at(data, load_addr, target)
    if first in (None, 0) or not _is_valid_mips_word(first):
        return False
    if _dense_local_pointer_table(data, load_addr, target):
        return False
    cap = min(load_addr + size, producer_hi, target + 0x4000)
    work = deque([target])
    visited = set()
    saw_return = False
    bad = False

    def inside(addr: int) -> bool:
        return target <= addr < cap and (addr & 3) == 0

    def add_delay(pc: int) -> None:
        nonlocal bad
        delay = pc + 4
        if not inside(delay):
            bad = True
            return
        word = _word_at(data, load_addr, delay)
        if not _is_valid_mips_word(word):
            bad = True
        else:
            visited.add(delay)

    while work and not bad and len(visited) < 2048:
        pc = work.popleft()
        if pc in visited:
            continue
        if not inside(pc):
            bad = True  # sequential fallthrough past the bounded probe
            break
        word = _word_at(data, load_addr, pc)
        if not _is_valid_mips_word(word):
            bad = True
            break
        visited.add(pc)
        kind, branch_target = _classify_cf(pc, word)
        if kind == 'normal':
            work.append(pc + 4)
        elif kind == 'branch':
            add_delay(pc)
            work.append(pc + 8)  # conditional fallthrough must remain bounded
            if inside(branch_target):
                work.append(branch_target)
            # An out-of-probe branch target is a shared external code edge.
        elif kind == 'branch_never':
            add_delay(pc)
            work.append(pc + 8)
        elif kind == 'branch_never_likely':
            work.append(pc + 8)
        elif kind in ('jal', 'jalr'):
            add_delay(pc)
            work.append(pc + 8)
        elif kind == 'j':
            add_delay(pc)
            if inside(branch_target):
                work.append(branch_target)
            # Otherwise this is a bounded tail edge.
        elif kind == 'jr_ra':
            add_delay(pc)
            saw_return = True
        elif kind == 'jr':
            add_delay(pc)  # bounded indirect/tail exit

    return saw_return and not bad and not work and len(visited) < 2048


def _walk_overlay_function(data: bytes, load_addr: int, size: int,
                           entry: int, hard_cap: int,
                           producer_ranges=(),
                           allow_cross_producer_calls=True) -> dict:
    lo = load_addr
    hi = load_addr + size

    work = deque([entry])
    visited = set()
    direct_jals = set()
    static_indirect_targets = set()
    call_continuations = set()
    branch_targets = set()
    jump_table_targets = set()
    jump_table_proofs = []
    forward_branch_targets = set()
    rejected_cross_producer_calls = set()
    accepted_cross_producer_calls = set()

    def producer_for(addr: int):
        for range_lo, range_hi in producer_ranges:
            if range_lo <= addr < range_hi:
                return range_lo, range_hi
        return None

    entry_producer = producer_for(entry)

    def in_function(addr: int) -> bool:
        return (lo <= addr < hi and entry <= addr < hard_cap and
                (addr & 3) == 0 and
                (not producer_ranges or
                 (entry_producer is not None and
                  producer_for(addr) == entry_producer)))

    def callable_transfer_target(source: int, target: int) -> bool:
        if _dense_local_pointer_table(data, load_addr, target):
            rejected_cross_producer_calls.add(target)
            return False
        if not producer_ranges:
            return True
        source_range = producer_for(source)
        target_range = producer_for(target)
        if source_range is None or target_range is None:
            rejected_cross_producer_calls.add(target)
            return False
        if source_range is not None and source_range == target_range:
            return True
        if not allow_cross_producer_calls:
            rejected_cross_producer_calls.add(target)
            return False
        # Adjacent-producer composites are a byte envelope, not proof that a
        # call encoded by producer A is linked to arbitrary bytes at the same
        # address in every producer-B variant. Cross-boundary calls therefore
        # need independent local boundary evidence. This keeps real exports
        # while rejecting pointer/data words that happen to be valid MIPS.
        if _callable_direct_jal_target(data, load_addr, target):
            accepted_cross_producer_calls.add(target)
            return True
        if (target_range is not None and
                plausible_callable_target(
                    data, load_addr, size, target, target_range[1])):
            accepted_cross_producer_calls.add(target)
            return True
        rejected_cross_producer_calls.add(target)
        return False

    while work:
        pc = work.popleft()
        if pc in visited or not in_function(pc):
            continue
        word = _word_at(data, load_addr, pc)
        if word is None:
            continue
        visited.add(pc)
        kind, target = _classify_cf(pc, word)
        delay = pc + 4

        if kind == 'normal':
            if in_function(pc + 4):
                work.append(pc + 4)
        elif kind == 'branch':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
            if in_function(target):
                work.append(target)
                branch_targets.add(target)
            elif lo <= target < hi and target >= hard_cap and (target & 3) == 0:
                # A reachable direct branch may jump across a sibling entry.
                # Exact-entry partitioning hard-caps the current walk there,
                # but the target remains mechanically proven code reachability.
                forward_branch_targets.add(target)
        elif kind == 'branch_never':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
        elif kind == 'branch_never_likely':
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
        elif kind == 'j':
            if in_function(delay):
                visited.add(delay)
            if in_function(target):
                work.append(target)
                branch_targets.add(target)
            elif lo <= target < hi and target >= hard_cap and (target & 3) == 0:
                forward_branch_targets.add(target)
        elif kind == 'jal':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
                call_continuations.add(pc + 8)
            if (lo <= target < hi and (target & 3) == 0 and
                    _is_valid_mips_word(_word_at(data, load_addr, target)) and
                    callable_transfer_target(pc, target)):
                direct_jals.add(target)
        elif kind == 'jalr':
            if in_function(delay):
                visited.add(delay)
            if in_function(pc + 8):
                work.append(pc + 8)
                branch_targets.add(pc + 8)
                call_continuations.add(pc + 8)
            target_reg = (word >> 21) & 0x1F
            target = _resolve_constant_transfer(data, load_addr, entry, pc,
                                                target_reg)
            if (target is not None and lo <= target < hi and (target & 3) == 0
                    and callable_transfer_target(pc, target)):
                static_indirect_targets.add(target)
        elif kind == 'jr':
            if in_function(delay):
                visited.add(delay)
            jr_rs = (word >> 21) & 0x1F
            target = _resolve_constant_transfer(data, load_addr, entry, pc, jr_rs)
            if target is not None and lo <= target < hi and (target & 3) == 0:
                if in_function(target):
                    branch_targets.add(target)
                    work.append(target)
                elif callable_transfer_target(pc, target):
                    static_indirect_targets.add(target)
            else:
                proofs = []
                for jt in _find_jump_table_targets(data, load_addr, size,
                                                   entry, hard_cap, pc, jr_rs,
                                                   entry_producer, proofs):
                    jump_table_targets.add(jt)
                    branch_targets.add(jt)
                    if in_function(jt):
                        work.append(jt)
                jump_table_proofs.extend(proofs)
        elif kind == 'jr_ra':
            if in_function(delay):
                visited.add(delay)

    return {
        'visited': visited,
        'direct_jals': direct_jals,
        'static_indirect_targets': static_indirect_targets,
        'call_continuations': call_continuations,
        'branch_targets': branch_targets,
        'jump_table_targets': jump_table_targets,
        'jump_table_proofs': jump_table_proofs,
        'forward_branch_targets': forward_branch_targets,
        'rejected_cross_producer_calls': rejected_cross_producer_calls,
        'accepted_cross_producer_calls': accepted_cross_producer_calls,
    }


def _collect_toml_overlay_entries(toml_doc: dict, load_addr: int, crc32: int) -> set[int]:
    entries = set()
    for ov in toml_doc.get('overlays', []) or []:
        if not isinstance(ov, dict):
            continue
        ov_load = ov.get('load_addr') or ov.get('load_address')
        if ov_load is not None and _parse_addr(ov_load) != load_addr:
            continue
        ov_crc = ov.get('bytes_crc') or ov.get('crc32') or ov.get('crc')
        if ov_crc is not None and _parse_addr(ov_crc) != crc32:
            continue
        for key in ('entry', 'entries', 'function_entry_pcs', 'function_entries'):
            if key not in ov:
                continue
            val = ov[key]
            if isinstance(val, list):
                entries.update(_parse_addr_list(val))
            else:
                entries.add(_parse_addr(val))
    return entries


def classify_overlay_seeds(cap: dict, data: bytes, load_addr: int, size: int,
                           crc32: int, toml_doc: dict) -> tuple[list[str], dict]:
    lo = load_addr
    hi = load_addr + size
    region = lambda a: lo <= a < hi and (a & 3) == 0
    producer_ranges = []
    for raw_range in cap.get('producer_ranges', []) or []:
        if not isinstance(raw_range, dict):
            raise RuntimeError('producer_ranges entries must be objects')
        try:
            range_lo = _parse_addr(raw_range['start'])
            range_hi = _parse_addr(raw_range['end'])
        except (KeyError, TypeError, ValueError) as exc:
            raise RuntimeError(f'invalid producer_ranges entry: {raw_range}') from exc
        if not (lo <= range_lo < range_hi <= hi):
            raise RuntimeError(
                f'producer range 0x{range_lo:08X}..0x{range_hi:08X} '
                f'outside capture 0x{lo:08X}..0x{hi:08X}')
        producer_ranges.append((range_lo, range_hi))
    producer_ranges.sort()
    if any(producer_ranges[i-1][1] > producer_ranges[i][0]
           for i in range(1, len(producer_ranges))):
        raise RuntimeError('producer_ranges overlap')

    def capture_producer_for(addr: int):
        for producer_lo, producer_hi in producer_ranges:
            if producer_lo <= addr < producer_hi:
                return producer_lo, producer_hi
        return None

    static_alias_ranges = set()
    allow_cross_producer_calls = not bool(cap.get('strict_producer_ranges'))
    for raw_alias in cap.get('static_alias_ranges', []) or []:
        if not isinstance(raw_alias, dict):
            raise RuntimeError('static_alias_ranges entries must be objects')
        try:
            entry = _parse_addr(raw_alias['entry'])
            range_lo = _parse_addr(raw_alias['start'])
            range_hi = _parse_addr(raw_alias['end'])
        except (KeyError, TypeError, ValueError) as exc:
            raise RuntimeError(
                f'invalid static_alias_ranges entry: {raw_alias}') from exc
        if not (lo <= range_lo <= entry < range_hi <= hi and
                (entry & 3) == 0 and (range_lo & 3) == 0 and
                (range_hi & 3) == 0):
            raise RuntimeError(
                f'static alias 0x{entry:08X} -> '
                f'0x{range_lo:08X}..0x{range_hi:08X} outside/misaligned for '
                f'capture 0x{lo:08X}..0x{hi:08X}')
        if producer_ranges:
            owners = [index for index, (producer_lo, producer_hi) in
                      enumerate(producer_ranges)
                      if (producer_lo <= range_lo < producer_hi and
                          producer_lo <= entry < producer_hi and
                          producer_lo <= range_hi - 1 < producer_hi)]
            if not owners:
                raise RuntimeError(
                    f'static alias 0x{entry:08X} crosses producer boundary')
        static_alias_ranges.add((entry, range_lo, range_hi))

    schema_keys = {'executed_pcs', 'observed_pcs', 'dispatch_entry_pcs',
                   'static_dispatch_entry_pcs', 'function_entry_pcs'}
    has_split_schema = any(k in cap for k in schema_keys)

    legacy_seeds = _parse_addr_list(cap.get('seeds', []))
    executed_pcs = _parse_addr_list(cap.get('executed_pcs',
                                            cap.get('observed_pcs',
                                                    legacy_seeds if not has_split_schema else [])))
    dispatch_entry_pcs = _parse_addr_list(cap.get('dispatch_entry_pcs', []))
    # New extractors preserve which dispatch entries came from a static
    # export/header/jump table.  Treat malformed superset values as ordinary
    # unknown metadata, never as cross-variant authority.
    declared_static_dispatch = _parse_addr_list(
        cap.get('static_dispatch_entry_pcs', []))
    if not declared_static_dispatch <= dispatch_entry_pcs:
        extra = sorted(declared_static_dispatch - dispatch_entry_pcs)
        raise RuntimeError(
            'static_dispatch_entry_pcs must be a subset of '
            'dispatch_entry_pcs: ' +
            ', '.join(f'0x{addr:08X}' for addr in extra))
    static_dispatch_entries = declared_static_dispatch
    captured_function_entries = _parse_addr_list(cap.get('function_entry_pcs', []))
    static_discovery_entries = _parse_addr_list(
        cap.get('static_discovery_entry_pcs', []))
    toml_entries = _collect_toml_overlay_entries(toml_doc, load_addr, crc32)
    legacy_callable_seeds = {a for a in legacy_seeds
                             if region(a) and _callable_legacy_seed(data, load_addr, a)}

    included: dict[int, str] = {}
    excluded: dict[int, str] = {}
    game_text = _game_text_range(toml_doc)

    # A dispatch into dirty RAM can land on a jump-table case label. That
    # proves coverage, not a callable function boundary, so discover those
    # labels before promoting dispatch entries to seeds.
    pre_roots = (set(legacy_callable_seeds) | set(captured_function_entries) |
                 set(static_discovery_entries) | set(toml_entries))
    pre_roots.update(a for a in dispatch_entry_pcs
                     if region(a) and _callable_legacy_seed(data, load_addr, a))
    jump_table_targets = set()
    pre_roots_sorted = sorted(a for a in pre_roots if region(a))
    for i, entry in enumerate(pre_roots_sorted):
        hard_cap = pre_roots_sorted[i + 1] if i + 1 < len(pre_roots_sorted) else hi
        walk = _walk_overlay_function(
            data, load_addr, size, entry, hard_cap, producer_ranges,
            allow_cross_producer_calls)
        jump_table_targets.update(walk['jump_table_targets'])

    def impossible_entry_start(addr: int) -> bool:
        word = _word_at(data, load_addr, addr)
        if word is None:
            return True
        if not _is_valid_mips_word(word):
            return True
        kind, target = _classify_cf(addr, word)
        if kind == 'j':
            return _classify_target(target, load_addr, size, game_text) == 'UNKNOWN_BAD'
        if kind == 'jal' and region(target):
            return not _callable_direct_jal_target(data, load_addr, target)
        return False

    def include(addr: int, reason: str):
        if not region(addr):
            excluded[addr] = 'UNKNOWN'
            return
        # Composite padding has bytes but no producer identity.  A root there
        # would otherwise get entry_producer=None and could walk through every
        # disconnected zero-fill gap as though those gaps shared ownership.
        if producer_ranges and capture_producer_for(addr) is None:
            excluded[addr] = 'UNKNOWN'
            return
        # Boundary gate: a dispatched-to PC is proof of *code reachability*, not
        # of a *callable function boundary*. A jr-driven jump-table case label
        # or a mid-function pointer target can be dispatched-to yet have no
        # prologue and no preceding jr $ra. Promoting it to a WALK ROOT would
        # truncate its host function (mid-function-seed softlock class) or run
        # off into adjacent data. Such PCs are dispatch-proven code, though —
        # classify them DISPATCH_INTERIOR: the recompiler emits them as
        # overlapping-alias entries into their host (never walk roots).
        # Invalid words stay excluded. Call-edge-proven reasons
        # (DIRECT_JAL_TARGET, FUNCTION_POINTER_TARGET, TOML_DECLARED_ENTRY) are
        # exempt — they carry their own proof.
        if reason in ('DISPATCH_ENTRY', 'STATIC_DISPATCH_ENTRY'):
            if impossible_entry_start(addr):
                excluded[addr] = 'UNKNOWN'
                return
            if (addr in jump_table_targets or
                    not _callable_legacy_seed(data, load_addr, addr)):
                included.setdefault(addr, 'DISPATCH_INTERIOR')
                return
        elif (reason not in ('DIRECT_JAL_TARGET', 'STATIC_INDIRECT_TARGET',
                             'STATIC_BRANCH_ROOT',
                             'STATIC_DISCOVERY_ROOT',
                             'TOML_DECLARED_ENTRY') and
                addr in jump_table_targets and
                not _callable_legacy_seed(data, load_addr, addr)):
            excluded[addr] = 'BRANCH_TARGET_ONLY'
            return
        if reason in FATAL_SEED_REASONS:
            raise RuntimeError(f'BUG: refusing to include 0x{addr:08X} as {reason}')
        old = included.get(addr)
        if reason in ('DIRECT_JAL_TARGET', 'STATIC_INDIRECT_TARGET',
                      'STATIC_BRANCH_ROOT', 'STATIC_DISCOVERY_ROOT',
                      'STATIC_DISPATCH_ENTRY'):
            # Static call-edge evidence outranks heuristic capture metadata.
            included[addr] = reason
        elif old is None or old in ('FUNCTION_POINTER_TARGET', 'DISPATCH_INTERIOR'):
            included[addr] = reason

    for addr in dispatch_entry_pcs:
        include(addr, ('STATIC_DISPATCH_ENTRY'
                       if addr in static_dispatch_entries
                       else 'DISPATCH_ENTRY'))
    for addr in captured_function_entries:
        include(addr, 'FUNCTION_POINTER_TARGET')
    for addr in static_discovery_entries:
        if not region(addr):
            excluded[addr] = 'UNKNOWN'
        elif (_callable_legacy_seed(data, load_addr, addr) or
              plausible_callable_target(data, load_addr, size, addr, hi)):
            include(addr, 'STATIC_DISCOVERY_ROOT')
        else:
            # Preserve the weaker classification. Overlay mode will perform
            # its ordinary boundary recheck and fail closed if needed.
            include(addr, 'FUNCTION_POINTER_TARGET')
    for addr in toml_entries:
        include(addr, 'TOML_DECLARED_ENTRY')

    legacy_seed_mode = bool(legacy_seeds) and not cap.get('schema')
    if legacy_seed_mode:
        # Old capture files used `seeds` for interpreter-observed PCs. Include
        # only PCs whose surrounding bytes look callable; classify the rest
        # after we know branch targets from accepted roots.
        for addr in legacy_callable_seeds:
            include(addr, 'FUNCTION_POINTER_TARGET')

    # Current-capture alias recipes contribute only an interior ENTRY candidate;
    # their historical host range is not trusted. Final exact reachability below
    # must discover a current host or the entry is dropped to the interpreter.
    for addr, _range_lo, _range_hi in static_alias_ranges:
        if region(addr):
            included[addr] = 'DISPATCH_INTERIOR'

    # Walk roots: callable entries only. DISPATCH_INTERIOR addresses are NOT
    # roots — as roots they would hard-cap (truncate) the sibling walk that
    # owns them.
    known = {a for a, r in included.items() if r != 'DISPATCH_INTERIOR'}
    initial_known = set(known)
    derived_reasons = {}
    promoted_roots = set()
    suppressed_derived = set()
    suppression_passes = 0
    all_branch_targets = set()
    kernel_window = (load_addr & 0x1FFFFFFF) < 0x10000

    while True:
        # Recompute call/branch evidence from the CURRENT partition. Evidence is
        # not monotonic: a new hard cap can make an old source PC unreachable.
        # Dropping stale candidates here prevents an old speculative walk from
        # resurrecting a target in the final shard.
        discovery_round = 0
        seen_states = {}
        state_history = []
        while True:
            discovery_round += 1
            state = (tuple(sorted(known)), tuple(sorted(derived_reasons)))
            cycle_start = seen_states.get(state)
            if discovery_round > 64 or cycle_start is not None:
                unstable = set()
                if cycle_start is not None:
                    cycle_states = state_history[cycle_start:]
                    known_sets = [set(item[0]) for item in cycle_states]
                    derived_sets = [set(item[1]) for item in cycle_states]
                    if known_sets:
                        unstable |= set.union(*known_sets) - set.intersection(*known_sets)
                    if derived_sets:
                        unstable |= (set.union(*derived_sets) -
                                     set.intersection(*derived_sets))
                unstable_text = ','.join(f'{addr:08X}'
                                         for addr in sorted(unstable)[:12])
                unstable -= initial_known | promoted_roots
                if unstable and suppression_passes < 64:
                    suppression_passes += 1
                    suppressed_derived.update(unstable)
                    print('  WARNING: ownership discovery cycle; suppressing '
                          f'{len(unstable)} unstable derived roots'
                          + (f' ({unstable_text})' if unstable_text else ''))
                    known = initial_known | promoted_roots
                    derived_reasons = {}
                    discovery_round = 0
                    seen_states = {}
                    state_history = []
                    continue
                print('  WARNING: ownership discovery did not converge; '
                      'falling back to explicit/dispatch roots'
                      + (f' (cycle={len(state_history) - cycle_start}, '
                         f'unstable={unstable_text})'
                         if cycle_start is not None else ' (round limit)'))
                known = initial_known | promoted_roots
                derived_reasons = {}
                break
            seen_states[state] = len(state_history)
            state_history.append(state)
            next_reasons = {}
            round_branch_targets = set()
            sorted_known = sorted(known)
            for index, entry in enumerate(sorted_known):
                hard_cap = (sorted_known[index + 1]
                            if index + 1 < len(sorted_known) else hi)
                walk = _walk_overlay_function(
                    data, load_addr, size, entry, hard_cap, producer_ranges,
                    allow_cross_producer_calls)
                round_branch_targets.update(walk['branch_targets'])
                round_branch_targets.update(walk['jump_table_targets'])
                for target in walk['direct_jals']:
                    if (target not in initial_known and
                            target not in suppressed_derived):
                        next_reasons[target] = 'DIRECT_JAL_TARGET'
                for target in walk['static_indirect_targets']:
                    if (target not in initial_known and
                            target not in suppressed_derived):
                        next_reasons.setdefault(target, 'STATIC_INDIRECT_TARGET')
                for target in walk['forward_branch_targets']:
                    source_reason = (included.get(entry) or
                                     derived_reasons.get(entry) or
                                     ('DISPATCH_ROOT'
                                      if entry in promoted_roots else None))
                    strong_source = source_reason in (
                        'DIRECT_JAL_TARGET', 'STATIC_INDIRECT_TARGET',
                        'STATIC_BRANCH_ROOT', 'STATIC_DISCOVERY_ROOT',
                        'STATIC_DISPATCH_ENTRY', 'TOML_DECLARED_ENTRY',
                        'DISPATCH_ROOT')
                    if not strong_source and not _callable_legacy_seed(
                            data, load_addr, entry):
                        continue
                    source_range = next(
                        ((rlo, rhi) for rlo, rhi in producer_ranges
                         if rlo <= entry < rhi), None)
                    target_range = next(
                        ((rlo, rhi) for rlo, rhi in producer_ranges
                         if rlo <= target < rhi), None)
                    if producer_ranges and source_range != target_range:
                        continue
                    producer_hi = target_range[1] if target_range else hi
                    if plausible_callable_target(
                            data, load_addr, size, target, producer_hi):
                        if (target not in initial_known and
                                target not in suppressed_derived):
                            next_reasons.setdefault(target, 'STATIC_BRANCH_ROOT')

            normalized = initial_known | promoted_roots | set(next_reasons)
            roots = sorted(normalized)
            for target in sorted(next_reasons):
                if target not in normalized:
                    continue
                index = bisect_left(roots, target)
                if index == len(roots) or roots[index] != target or index == 0:
                    continue
                owner = roots[index - 1]
                hard_cap = roots[index + 1] if index + 1 < len(roots) else hi
                owner_walk = _walk_overlay_function(
                    data, load_addr, size, owner, hard_cap, producer_ranges,
                    allow_cross_producer_calls)
                if target in owner_walk['visited']:
                    normalized.remove(target)
                    roots.pop(index)

            if normalized == known and next_reasons == derived_reasons:
                all_branch_targets = round_branch_targets
                break
            known = normalized
            derived_reasons = next_reasons

        if not kernel_window:
            break

        # Orphan promotion (see DISPATCH_ROOT in INCLUDE_REASONS): a kernel
        # dispatch interior that no rooted walk covers is promoted to a
        # trusted walk root. Re-enter the walk loop — the new root's walk
        # may cover other interiors or discover direct-jal callees.
        covered = set()
        sorted_known = sorted(known)
        for i, entry in enumerate(sorted_known):
            hard_cap = sorted_known[i + 1] if i + 1 < len(sorted_known) else hi
            walk = _walk_overlay_function(
                data, load_addr, size, entry, hard_cap, producer_ranges,
                allow_cross_producer_calls)
            covered |= walk['visited']
        promoted = sorted(a for a, r in included.items()
                          if r == 'DISPATCH_INTERIOR' and a not in covered
                          and _is_valid_mips_word(_word_at(data, load_addr, a)))
        if not promoted:
            break
        # Promote ONE orphan per iteration (lowest address first): its walk
        # usually covers the remaining orphans, which then stay interiors and
        # alias into the new root as host — rather than minting sibling roots
        # that split one real function and hard-cap each other.
        a = promoted[0]
        included[a] = 'DISPATCH_ROOT'
        promoted_roots.add(a)
        known.add(a)

    # Re-walk with the final function set so the branch-target exclusion count
    # matches the actual compilation boundaries.
    all_branch_targets.clear()
    rejected_cross_producer_calls = set()
    accepted_cross_producer_calls = set()
    final_covered = set()
    sorted_known = sorted(known)
    for i, entry in enumerate(sorted_known):
        hard_cap = sorted_known[i + 1] if i + 1 < len(sorted_known) else hi
        walk = _walk_overlay_function(
            data, load_addr, size, entry, hard_cap, producer_ranges,
            allow_cross_producer_calls)
        all_branch_targets.update(walk['branch_targets'])
        all_branch_targets.update(walk['jump_table_targets'])
        all_branch_targets.update(walk['forward_branch_targets'])
        final_covered.update(walk['visited'])
        rejected_cross_producer_calls.update(
            walk['rejected_cross_producer_calls'])
        accepted_cross_producer_calls.update(
            walk['accepted_cross_producer_calls'])

    # A DISPATCH_INTERIOR is emitted only when the final partition has a real
    # CFG host for that exact PC. Reconsider every derived target after all
    # later roots have settled; stale ownership falls back to a root (when it
    # remains call-proven) or to the interpreter, never a hostless alias.
    for addr, reason in sorted(derived_reasons.items()):
        if addr in known:
            include(addr, reason)
        elif addr in final_covered:
            included[addr] = 'DISPATCH_INTERIOR'
        else:
            included.pop(addr, None)
            excluded[addr] = 'OBSERVED_PC_ONLY'
    for addr, reason in list(included.items()):
        if reason == 'DISPATCH_INTERIOR' and addr not in final_covered:
            del included[addr]
            excluded[addr] = ('OBSERVED_PC_ONLY'
                              if addr in executed_pcs else 'UNKNOWN')

    candidates = {a for a in (executed_pcs | dispatch_entry_pcs |
                              captured_function_entries | static_discovery_entries |
                              legacy_seeds | toml_entries)
                  if region(a)}
    for addr in sorted(candidates - set(included)):
        if addr in all_branch_targets or addr in jump_table_targets:
            excluded[addr] = 'BRANCH_TARGET_ONLY'
        elif addr in executed_pcs or addr in legacy_seeds:
            excluded[addr] = 'OBSERVED_PC_ONLY'
        else:
            excluded[addr] = 'UNKNOWN'

    bad_included = [(a, r) for a, r in included.items() if r in FATAL_SEED_REASONS]
    if bad_included:
        details = ', '.join(f'0x{a:08X}:{r}' for a, r in bad_included)
        raise RuntimeError(f'fatal seed classification: {details}')

    counts = Counter(included.values())
    excluded_counts = Counter(excluded.values())
    audit = {
        'load_addr': load_addr,
        'crc32': crc32,
        'lo': lo,
        'hi': hi,
        'executed_pcs': executed_pcs,
        'dispatch_entry_pcs': dispatch_entry_pcs,
        'static_dispatch_entry_pcs': static_dispatch_entries,
        'function_entry_pcs': set(included),
        'included_reasons': included,
        'excluded_reasons': excluded,
        'branch_targets_excluded_count': len(all_branch_targets - set(included)),
        'counts': counts,
        'excluded_counts': excluded_counts,
        'producer_ranges': producer_ranges,
        'static_alias_ranges': static_alias_ranges,
        # Every strong walk root is an exact-entry demand. These roots come from
        # decoder/control-flow, callable-boundary, explicit TOML, or dispatch
        # proof rather than an arbitrary observed PC. If a shared-region build
        # (or a concurrent poorer winner) lacks its F entry, the post-pass can
        # safely retry it in the isolated-fragment failure domain.
        'static_exact_fragment_demands': {
            addr for addr, reason in included.items()
            if reason in EXACT_FRAGMENT_REASONS
        },
        # Only static control-flow/export evidence may nominate an address in a
        # sibling byte recipe. The recipient still has to prove an existing
        # CRC-valid host and the recompiler must prove a real block leader.
        'cross_variant_hosted_demands': {
            addr for addr, reason in included.items()
            if reason in CROSS_VARIANT_DONOR_REASONS
        },
        # Alias recipes describe entry labels inside a normal-mode host.  They
        # need an isolated body only when the stricter shared analysis leaves
        # the label outside every current code interval; aliases already inside
        # a guarded body remain candidates for the future exact-resume pass.
        'static_interval_fragment_demands': {
            entry for entry, _lo, _hi in static_alias_ranges
        },
        'rejected_cross_producer_calls': rejected_cross_producer_calls,
        'accepted_cross_producer_calls': accepted_cross_producer_calls,
        'static_jump_table_proofs': cap.get('static_jump_table_proofs', []),
        'static_call_continuation_pcs': _parse_addr_list(
            cap.get('static_call_continuation_pcs', [])),
    }
    # Interior entries carry the 'interior' marker so the recompiler emits
    # them as overlapping aliases, never as walk roots. Promoted kernel
    # orphans carry 'dispatch_root' so the recompiler roots them without
    # boundary re-verification.
    seeds = []
    for range_lo, range_hi in producer_ranges:
        seeds.append(f'producer_range 0x{range_lo:08X} 0x{range_hi:08X}')
    for addr in sorted(accepted_cross_producer_calls):
        seeds.append(f'cross_call_allow 0x{addr:08X}')
    seeds.extend(seed_line_for_reason(addr, included[addr])
                 for addr in sorted(included))
    return seeds, audit


def print_seed_audit(audit: dict) -> None:
    print(f'Overlay {audit["load_addr"]:08X}_{audit["crc32"]:08X}')
    print(f'Region: {audit["lo"]:08X}..{audit["hi"] - 1:08X}')
    print(f'executed_pcs: {len(audit["executed_pcs"])}')
    print(f'dispatch_entry_pcs: {len(audit["dispatch_entry_pcs"])}')
    print(f'function_entry_pcs: {len(audit["function_entry_pcs"])}')
    print(f'direct_jal_targets_included: {audit["counts"].get("DIRECT_JAL_TARGET", 0)}')
    print(f'static_indirect_targets_included: {audit["counts"].get("STATIC_INDIRECT_TARGET", 0)}')
    print(f'static_branch_roots_included: '
          f'{audit["counts"].get("STATIC_BRANCH_ROOT", 0)}')
    print(f'static_discovery_roots_included: '
          f'{audit["counts"].get("STATIC_DISCOVERY_ROOT", 0)}')
    print(f'function_pointer_targets_included: {audit["counts"].get("FUNCTION_POINTER_TARGET", 0)}')
    print(f'toml_entries_included: {audit["counts"].get("TOML_DECLARED_ENTRY", 0)}')
    print(f'dispatch_interior_included: {audit["counts"].get("DISPATCH_INTERIOR", 0)}')
    print(f'dispatch_roots_promoted: {audit["counts"].get("DISPATCH_ROOT", 0)}')
    print(f'cross_producer_calls_rejected: '
          f'{len(audit["rejected_cross_producer_calls"])}')
    print(f'cross_producer_calls_accepted: '
          f'{len(audit["accepted_cross_producer_calls"])}')
    print(f'branch_targets_excluded: {audit["branch_targets_excluded_count"]}')
    print(f'observed_only_excluded: {audit["excluded_counts"].get("OBSERVED_PC_ONLY", 0)}')
    print(f'unknown_excluded: {audit["excluded_counts"].get("UNKNOWN", 0)}')
    for addr in sorted(audit['included_reasons']):
        print(f'  {addr:08X}  {audit["included_reasons"][addr]}')
    for addr in sorted(audit['excluded_reasons']):
        reason = audit['excluded_reasons'][addr]
        if reason in ('BRANCH_TARGET_ONLY', 'OBSERVED_PC_ONLY', 'UNKNOWN'):
            print(f'  {addr:08X}  excluded: {reason}')


def walk_root_seed_entries(seeds: list[str]) -> set[int]:
    """Exact F entries demanded by recompiler walk-root seed records."""
    entries = set()
    for seed in seeds:
        parts = seed.split()
        if not parts:
            continue
        if (parts[0] not in ('call_root', 'dispatch_root') and
                not parts[0].startswith('0x')):
            continue
        try:
            entries.add((int(parts[-1], 16) & 0x1FFFFFFF) | 0x80000000)
        except ValueError:
            pass
    return entries


def _game_text_range(toml_doc: dict) -> tuple[int, int]:
    game = toml_doc.get('game', {})
    load = game.get('load_address')
    size = game.get('text_size')
    if load is None or size is None:
        return (0, 0)
    lo = _parse_addr(load) & 0x1FFFFFFF
    hi = lo + _parse_addr(size)
    return lo, hi


def _classify_target(addr: int, load_addr: int, size: int,
                     game_text: tuple[int, int]) -> str:
    phys = addr & 0x1FFFFFFF
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size
    if ov_lo <= phys < ov_hi:
        return 'INSIDE_OVERLAY'
    game_lo, game_hi = game_text
    if game_hi > game_lo and game_lo <= phys < game_hi:
        return 'MAIN_EXE'
    if 0x1FC00000 <= phys < 0x1FC80000 or 0xBFC00000 <= addr < 0xBFC80000:
        return 'BIOS'
    return 'UNKNOWN_BAD'


def audit_generated_c(src: str, load_addr: int, size: int,
                      crc32: int, toml_doc: dict) -> dict:
    defs = {int(x, 16) for x in re.findall(
        r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)$', src, re.MULTILINE)}
    decls = {int(x, 16) for x in re.findall(
        r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\);$', src, re.MULTILINE)}
    direct_calls = [int(x, 16) for x in re.findall(
        r'\bfunc_([0-9A-Fa-f]{8})\(cpu\)', src)]
    literal_cba = [int(x, 16) for x in re.findall(
        r'\bcall_by_address\(cpu,\s*0x([0-9A-Fa-f]{8})u?\)', src)]
    unsupported_todo_addrs = {int(x, 16) for x in re.findall(
        r'TODO:[^\n]*?0x([0-9A-Fa-f]{8}):', src)}

    game_text = _game_text_range(toml_doc)
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_overlay(addr: int) -> bool:
        phys = addr & 0x1FFFFFFF
        return ov_lo <= phys < ov_hi

    unknown_bad = set()
    missing_direct = set()
    direct_outside = set()
    for addr in direct_calls:
        if not in_overlay(addr):
            direct_outside.add(addr)
            unknown_bad.add(addr)
        elif addr not in defs:
            missing_direct.add(addr)
            unknown_bad.add(addr)

    cba_classes = Counter()
    for addr in literal_cba:
        cls = _classify_target(addr, load_addr, size, game_text)
        cba_classes[cls] += 1
        if cls == 'UNKNOWN_BAD':
            unknown_bad.add(addr)

    decl_without_def = {a for a in decls if in_overlay(a) and a not in defs}
    called_decl_without_def = decl_without_def & set(direct_calls)
    if called_decl_without_def:
        unknown_bad.update(called_decl_without_def)

    report = {
        'defs': defs,
        'decls': decls,
        'direct_calls': direct_calls,
        'literal_cba': literal_cba,
        'direct_inside': sum(1 for a in direct_calls if in_overlay(a)),
        'external_cba': sum(1 for a in literal_cba if not in_overlay(a)),
        'bios_calls': cba_classes.get('BIOS', 0),
        'syscall_uses': len(re.findall(r'\bpsx_syscall\s*\(', src)),
        'unknown_dispatch_uses': len(re.findall(r'\bpsx_unknown_dispatch\s*\(', src)),
        'unsupported_todo_addrs': unsupported_todo_addrs,
        'unknown_bad': unknown_bad,
        'direct_outside': direct_outside,
        'missing_direct': missing_direct,
        'decl_without_def': decl_without_def,
        'called_decl_without_def': called_decl_without_def,
        'cba_classes': cba_classes,
    }
    return report


def print_generated_c_audit(load_addr: int, size: int, crc32: int,
                            report: dict) -> None:
    print(f'Overlay {load_addr:08X}_{crc32:08X}')
    print(f'Region: {load_addr:08X}..{load_addr + size - 1:08X}')
    print(f'Function definitions: {len(report["defs"])}')
    print(f'Forward declarations: {len(report["decls"])}')
    print(f'Direct calls inside overlay: {report["direct_inside"]}')
    print(f'External calls via call_by_address: {report["external_cba"]}')
    print(f'BIOS calls: {report["bios_calls"]}')
    print(f'Syscall uses: {report["syscall_uses"]}')
    print(f'Unsupported instruction TODOs: {len(report["unsupported_todo_addrs"])}')
    print(f'Unknown/bad targets: {len(report["unknown_bad"])}')
    for addr in sorted(report['unknown_bad']):
        print(f'  0x{addr:08X} UNKNOWN_BAD')
    for addr in sorted(report['unsupported_todo_addrs'])[:20]:
        print(f'  0x{addr:08X} UNSUPPORTED_INSTRUCTION')
    if len(report['unsupported_todo_addrs']) > 20:
        print(f'  ... {len(report["unsupported_todo_addrs"]) - 20} more unsupported instructions')


# ---------------------------------------------------------------------------
# Post-process generated C for DLL compilation
# ---------------------------------------------------------------------------

# The overlay dispatch shim / link contract is the SINGLE SOURCE OF TRUTH for
# the symbols every compiled shard links against (dispatch forwarders, GTE/WS
# hooks, cycle callbacks). It lives beside the ABI header it must agree with,
# at <runtime_include>/overlay_dispatch_preamble.c.inc, so cmake can fold its
# content into the overlay cache tag (codegen_hash_sources.cmake) -- a change to
# the link contract then auto-invalidates the cache instead of silently
# breaking every shard compile against a moved-on runtime. Loaded once by
# load_dispatch_preamble(); patch_generated_c() reads this global.
DISPATCH_PREAMBLE = None
PREAMBLE_INC_NAME = "overlay_dispatch_preamble.c.inc"


def load_dispatch_preamble(runtime_include: str) -> str:
    """Load the shard dispatch-shim preamble from <runtime_include>/<inc> and
    cache it in the module global. Missing is a HARD error: a shard built
    without the shim links against nothing, and silently skipping it is exactly
    the invisible-failure class this tooling exists to eliminate."""
    global DISPATCH_PREAMBLE
    if DISPATCH_PREAMBLE is not None:
        return DISPATCH_PREAMBLE
    path = os.path.join(runtime_include, PREAMBLE_INC_NAME)
    try:
        with open(path, encoding="utf-8") as f:
            DISPATCH_PREAMBLE = f.read()
    except OSError as e:
        raise SystemExit(
            f"FATAL: cannot read overlay dispatch preamble {path}: {e}\n"
            f"  This file is the shard link contract and MUST ship in "
            f"runtime/include. Without it no shard can be compiled.")
    return DISPATCH_PREAMBLE

def patch_generated_c(src: str, load_addr: int, size: int) -> str:
    """
    Post-process psxrecomp-game's _full.c output for standalone DLL compilation:

    1. Prepend the dispatch function-pointer preamble (before any includes).
    2. Remove forward declarations for functions outside the overlay range —
       they'd be unresolved externals in the DLL.
    3. Replace direct calls to out-of-range func_XXXXXXXX(cpu) with
       call_by_address(cpu, 0xXXXXXXXXu) so they go through the dispatch ptr.
    4. Export all func_XXXXXXXX symbols so the runtime can enumerate them.
    """
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_overlay(addr: int) -> bool:
        phys = addr & 0x1FFFFFFF
        return ov_lo <= phys < ov_hi

    # 1. Insert preamble AFTER the last #include line (so CPUState is complete)
    last_inc = -1
    for m in re.finditer(r'^#include\s+[<"].*[>"]\s*$', src, re.MULTILINE):
        last_inc = m.end()
    if last_inc == -1:
        src = DISPATCH_PREAMBLE + src
    else:
        src = src[:last_inc] + '\n' + DISPATCH_PREAMBLE + src[last_inc:]

    # 2. Remove out-of-range forward declarations
    def drop_extern(m):
        addr = int(m.group(1), 16)
        return '' if not in_overlay(addr) else m.group(0)
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\);\n',
                 drop_extern, src, flags=re.MULTILINE)

    # 3. Replace out-of-range direct calls with call_by_address
    def fix_call(m):
        addr = int(m.group(1), 16)
        if in_overlay(addr):
            return m.group(0)
        return f'call_by_address(cpu, 0x{addr:08X}u)'
    src = re.sub(r'\bfunc_([0-9A-Fa-f]{8})\(cpu\)',
                 fix_call, src)

    # 4. Add dllexport to every in-overlay func_XXXXXXXX definition.
    #    psxrecomp-game emits "void func_XXXXXXXX(CPUState* cpu)" on one line
    #    with "{" on the NEXT line — match the signature line alone (no ";" = definition).
    def add_export(m):
        addr = int(m.group(1), 16)
        if not in_overlay(addr):
            return m.group(0)
        return (
            '#ifdef _WIN32\n__declspec(dllexport)\n#else\n'
            '__attribute__((visibility("default")))\n#endif\n'
            + m.group(0)
        )
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)$',
                 add_export, src, flags=re.MULTILINE)

    return src


# ---------------------------------------------------------------------------
# Static (B-2) post-processing
# ---------------------------------------------------------------------------

STATIC_PREAMBLE = """\
/* ---- Static overlay (B-2): psx_runtime.h already provides call_by_address. */

"""

def patch_generated_c_static(src: str, load_addr: int, size: int) -> tuple:
    """
    Post-process psxrecomp-game's _full.c output for static binary compilation.

    Returns (patched_src, sorted_list_of_in_overlay_virt_addrs).

    Differences from DLL path:
    - No overlay_api.h / OverlayCallbacks / overlay_init
    - Callbacks are direct extern calls, not function pointers
    - No __declspec(dllexport) — all functions have normal external linkage
    - Returns function address list so caller can build the switch dispatch
    """
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_overlay(addr: int) -> bool:
        return ov_lo <= (addr & 0x1FFFFFFF) < ov_hi

    # 1. Insert preamble after last #include
    last_inc = -1
    for m in re.finditer(r'^#include\s+[<"].*[>"]\s*$', src, re.MULTILINE):
        last_inc = m.end()
    if last_inc == -1:
        src = STATIC_PREAMBLE + src
    else:
        src = src[:last_inc] + '\n' + STATIC_PREAMBLE + src[last_inc:]

    # 2. Remove out-of-range forward declarations
    def drop_extern(m):
        addr = int(m.group(1), 16)
        return '' if not in_overlay(addr) else m.group(0)
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\);\n',
                 drop_extern, src, flags=re.MULTILINE)

    # 3. Replace out-of-range direct calls with call_by_address
    def fix_call(m):
        addr = int(m.group(1), 16)
        if in_overlay(addr):
            return m.group(0)
        return f'call_by_address(cpu, 0x{addr:08X}u)'
    src = re.sub(r'\bfunc_([0-9A-Fa-f]{8})\(cpu\)', fix_call, src)

    # 4. Collect in-overlay function definition addresses (no export annotation needed)
    func_virt_addrs = []
    def collect_fn(m):
        addr = int(m.group(1), 16)
        if in_overlay(addr):
            func_virt_addrs.append(0x80000000 | (addr & 0x1FFFFFFF))
        return m.group(0)
    src = re.sub(r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)$',
                 collect_fn, src, flags=re.MULTILINE)

    return src, sorted(func_virt_addrs)


def namespace_generated_static(src: str, namespace: str,
                               func_virt_addrs: list) -> tuple:
    """Give one generated overlay image private C symbols.

    Static mode combines several independently-generated C files into one
    translation unit. The recompiler deliberately gives each file the same
    helper names, and different overlay images may define functions at the same
    guest address. Namespace the four common helpers, every in-image function,
    and every alias-body helper. Return the guest-entry -> C-symbol map used by
    the content-validated dispatcher.
    """
    func_set = set(func_virt_addrs)

    for helper in ('psx_lwl', 'psx_lwr', 'psx_swl', 'psx_swr'):
        src = re.sub(rf'\b{helper}\b', f'{namespace}_{helper}', src)

    def rename_func(m):
        addr = (int(m.group(1), 16) & 0x1FFFFFFF) | 0x80000000
        if addr not in func_set:
            return m.group(0)
        return f'{namespace}_func_{addr:08X}'

    src = re.sub(r'\bfunc_([0-9A-Fa-f]{8})\b', rename_func, src)

    def rename_alias(m):
        addr = (int(m.group(1), 16) & 0x1FFFFFFF) | 0x80000000
        if addr not in func_set:
            return m.group(0)
        return f'{namespace}_alias_body_{addr:08X}'

    src = re.sub(r'\bpsx_alias_body_([0-9A-Fa-f]{8})\b', rename_alias, src)
    symbols = {va: f'{namespace}_func_{va:08X}' for va in func_set}
    return src, symbols


def parse_cps_continuation_owners(src: str) -> dict:
    """Return compiled block entry -> owning function for generated CPS C.

    Runtime captures can prove a block label as a dispatch entry even when an
    earlier capture never needed that label in the host's ``cpu->pc`` resume
    switch. Static mode can add the missing switch arm and synthesize a wrapper
    as long as the host's exact compiled code ranges match live RAM.
    """
    definition_re = re.compile(
        r'^void func_([0-9A-Fa-f]{8})\(CPUState\* cpu\)\n\{',
        re.MULTILINE)
    definitions = list(definition_re.finditer(src))
    owners = {}
    for index, match in enumerate(definitions):
        host = (int(match.group(1), 16) & 0x1FFFFFFF) | 0x80000000
        end = definitions[index + 1].start() if index + 1 < len(definitions) else len(src)
        body = src[match.end():end]
        for block in re.finditer(r'^block_([0-9A-Fa-f]{8}):', body,
                                 re.MULTILINE):
            entry = (int(block.group(1), 16) & 0x1FFFFFFF) | 0x80000000
            owners.setdefault(entry, host)
    return owners


def add_cps_resume_case(src: str, host_symbol: str,
                        host: int, entry: int) -> tuple:
    """Make ``entry`` a legal ``cpu->pc`` resume point in one native host."""
    definition = re.search(
        rf'^void {re.escape(host_symbol)}\(CPUState\* cpu\)\n\{{',
        src, re.MULTILINE)
    if not definition:
        return src, False
    next_definition = re.search(r'^void [A-Za-z_][A-Za-z0-9_]*\(CPUState\* cpu\)',
                                src[definition.end():], re.MULTILINE)
    end = (definition.end() + next_definition.start()
           if next_definition else len(src))
    segment = src[definition.start():end]
    if not re.search(rf'^block_{entry:08X}:', segment, re.MULTILINE):
        return src, False
    if f'case 0x{entry:08X}u: goto block_{entry:08X};' in segment:
        return src, True

    hook = segment.find('debug_server_log_call_entry')
    prologue = segment[:hook] if hook >= 0 else ''
    default_match = re.search(r'^\s+default:', prologue, re.MULTILINE)
    if 'if (cpu->pc != 0u)' in prologue and default_match:
        insert = definition.start() + default_match.start()
        indent = re.match(r'\s*', prologue[default_match.start():]).group(0)
        arm = f'{indent}case 0x{entry:08X}u: goto block_{entry:08X};\n'
        return src[:insert] + arm + src[insert:], True

    prologue_text = (
        '\n    if (cpu->pc != 0u) {\n'
        '        uint32_t _cont = cpu->pc; cpu->pc = 0;\n'
        '        switch (_cont) {\n'
        f'            case 0x{entry:08X}u: goto block_{entry:08X};\n'
        f'            case 0x{host:08X}u: break;  /* entry at prologue */\n'
        f'            default: cpu->pc = _cont; psx_native_bad_entry(cpu, '
        f'0x{host:08X}u, _cont); return;\n'
        '        }\n'
        '    }')
    return src[:definition.end()] + prologue_text + src[definition.end():], True


def generate_overlay_dispatch(variants: list) -> str:
    """Generate byte-validated dispatch for all static overlay variants."""
    unique = []
    seen = set()
    for variant in variants:
        ranges = tuple((lo & 0x1FFFFFFF, length)
                       for lo, length in variant['ranges'])
        key = (variant['addr'], variant['crc'], ranges)
        if key in seen:
            continue
        seen.add(key)
        item = dict(variant)
        item['ranges'] = ranges
        unique.append(item)

    unique.sort(key=lambda v: (v['addr'], v['crc'], v['ranges'], v['symbol']))
    by_addr = {}
    for index, variant in enumerate(unique):
        variant['range_symbol'] = f'psx_ov_static_ranges_{index:05d}'
        by_addr.setdefault(variant['addr'], []).append(variant)

    lines = [
        '',
        '/* Auto-generated, content-validated overlay dispatch -- do not edit. */',
        'extern int psx_overlay_static_code_matches(const uint32_t *lo_len_pairs,',
        '                                           uint32_t count,',
        '                                           uint32_t expected_crc);',
        'static uint64_t psx_ov_static_checks = 0;',
        'static uint64_t psx_ov_static_hits = 0;',
        'static uint64_t psx_ov_static_variant_misses = 0;',
        'static uint64_t psx_ov_static_address_misses = 0;',
        '',
    ]
    for variant in unique:
        flat = []
        for lo, length in variant['ranges']:
            flat.extend((f'0x{lo:08X}u', f'0x{length:X}u'))
        lines.append(
            f'static const uint32_t {variant["range_symbol"]}[] = '
            '{ ' + ', '.join(flat) + ' };')

    lines += [
        '',
        'void psx_overlay_static_get_stats(uint64_t *checks, uint64_t *hits,',
        '                                  uint64_t *variant_misses,',
        '                                  uint64_t *address_misses) {',
        '    if (checks) *checks = psx_ov_static_checks;',
        '    if (hits) *hits = psx_ov_static_hits;',
        '    if (variant_misses) *variant_misses = psx_ov_static_variant_misses;',
        '    if (address_misses) *address_misses = psx_ov_static_address_misses;',
        '}',
        '',
        'int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) {',
        '    const uint32_t key = (addr & 0x1FFFFFFFu) | 0x80000000u;',
        '    switch (key) {',
    ]
    for addr in sorted(by_addr):
        lines.append(f'        case 0x{addr:08X}u:')
        for variant in by_addr[addr]:
            count = len(variant['ranges'])
            lines += [
                '            psx_ov_static_checks++;',
                f'            if (psx_overlay_static_code_matches('
                f'{variant["range_symbol"]}, {count}u, '
                f'0x{variant["crc"]:08X}u)) {{',
                '                psx_ov_static_hits++;',
                f'                {variant["symbol"]}(cpu);',
                '                return 1;',
                '            }',
                '            psx_ov_static_variant_misses++;',
            ]
        lines.append('            return 0;')
    lines += [
        '        default:',
        '            psx_ov_static_address_misses++;',
        '            return 0;',
        '    }',
        '}',
        '',
    ]
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# DLL compilation
# ---------------------------------------------------------------------------

def parse_overlay_func_ids(src_path: str, data: bytes, load_addr: int,
                           size: int) -> list:
    """Parse the recompiler's _full.ranges manifest and return the list of
    in-overlay function identities as (ev, code_crc, ranges) tuples, where
    ev = virtual entry, code_crc = AUTHORITATIVE hash of the captured code-range
    bytes (the exact bytes the recompiler compiled from), and ranges is the
    coalesced [(lo, len), ...] code-range list.

    This (entry, code_crc) pair is the per-function IDENTITY: the loader marks a
    compiled entry callable iff live RAM matches code_crc, and overlay-cache v2
    keys build/dedup decisions on this set rather than on the whole-region CRC.

    binascii.crc32 (zlib, poly 0xEDB88320, init/final 0xFFFFFFFF) is bit-identical
    to the runtime's crc32_compute, and `data` is the raw little-endian RAM image,
    so the offline hash matches the runtime's hash of live RAM byte-for-byte."""
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size

    def in_ov(a: int) -> bool:
        return ov_lo <= (a & 0x1FFFFFFF) < ov_hi

    # Parse the recompiler manifest into [(entry, [(lo, len), ...]), ...].
    funcs: list[tuple[int, list[tuple[int, int]]]] = []
    cur = None
    with open(src_path) as f:
        for line in f:
            s = line.split()
            if not s:
                continue
            if s[0] == 'F':
                try:
                    addr = int(s[1], 16)
                except (IndexError, ValueError):
                    cur = None
                    continue
                if in_ov(addr):
                    cur = (addr, [])
                    funcs.append(cur)
                else:
                    cur = None
            elif s[0] == 'R' and cur is not None:
                try:
                    lo, length = int(s[1], 16), int(s[2], 16)
                except (IndexError, ValueError):
                    continue
                cur[1].append((lo, length))

    out = []
    for entry, ranges in funcs:
        if not ranges:
            continue
        crc = 0
        ok = True
        for lo, length in ranges:
            off = (lo & 0x1FFFFFFF) - ov_lo
            if off < 0 or off + length > len(data):
                ok = False  # range outside captured bytes — can't hash reliably
                break
            crc = binascii.crc32(data[off:off + length], crc)
        if not ok:
            continue
        ev = (entry & 0x1FFFFFFF) | 0x80000000
        out.append((ev, crc & 0xFFFFFFFF, ranges))

    return out


def _mips_control_kind(instr: int) -> int:
    """1=supported delayed transfer, 0=ordinary, -1=reserved/unsupported."""
    op = (instr >> 26) & 0x3F
    if op == 0:
        return int((instr & 0x3F) in (0x08, 0x09))  # JR/JALR
    if op == 0x01:
        return 1 if ((instr >> 16) & 0x1F) in (0x00, 0x01, 0x10, 0x11) else -1
    if op in (0x02, 0x03) or 0x04 <= op <= 0x07:
        return 1
    if 0x14 <= op <= 0x17:
        return -1
    if 0x10 <= op <= 0x13 and ((instr >> 21) & 0x1F) == 0x08:
        return -1
    return 0


def audit_func_id_delay_slots(func_ids: list, data: bytes,
                              load_addr: int) -> list:
    """Return unsafe emitted dependency errors for exact function identities."""
    errors = []
    base = load_addr & 0x1FFFFFFF
    for entry, _crc, ranges in func_ids:
        if len(ranges) > 16:
            errors.append((entry, None,
                           f'{len(ranges)} ranges exceed loader limit 16'))
            continue
        normalized = []
        malformed = False
        for lo, length in ranges:
            phys = lo & 0x1FFFFFFF
            off = phys - base
            if (phys & 3) or (length & 3) or length < 4 or \
                    off < 0 or off + length > len(data):
                errors.append((entry, phys | 0x80000000,
                               'malformed/out-of-capture code range'))
                malformed = True
                break
            normalized.append((phys, phys + length))
        if malformed:
            continue

        def contains_word(addr: int) -> bool:
            return any(lo <= addr and addr + 4 <= hi
                       for lo, hi in normalized)

        controls = set()
        for lo, hi in normalized:
            for pc in range(lo, hi, 4):
                off = pc - base
                instr = int.from_bytes(data[off:off + 4], 'little')
                kind = _mips_control_kind(instr)
                if kind < 0:
                    errors.append((entry, pc | 0x80000000,
                                   'reserved/unsupported branch encoding'))
                elif kind > 0:
                    controls.add(pc)
                    if not contains_word(pc + 4):
                        errors.append((entry, pc | 0x80000000,
                                       f'delay slot 0x{(pc + 4) | 0x80000000:08X} '
                                       'is not identity-hashed'))
        delay_pcs = {pc + 4 for pc in controls}
        nested = sorted(controls & delay_pcs)
        for pc in nested:
            errors.append((entry, pc | 0x80000000,
                           'control transfer in a delay slot'))
    return errors


def overlay_ranges_text(func_ids: list, pair_id: int | None = None,
                        provenance: str | None = None) -> str:
    """Serialize one loader manifest.

    ``P`` is an optional DLL/manifest publication identity.  Old runtimes
    ignore the unfamiliar record, while pair-aware runtimes require a matching
    ``overlay_pair_id`` export whenever it is present.
    """
    if provenance is not None and provenance not in \
            NON_AUTHORITY_MANIFEST_PROVENANCES:
        raise ValueError(f'unsupported overlay manifest provenance: {provenance}')
    out_lines = ['# psxrecomp overlay code-range manifest v2 (entry+code_crc)\n']
    if provenance is not None:
        out_lines.append(f'# psxrecomp overlay provenance {provenance}\n')
    if pair_id is not None:
        out_lines.append(f'P {pair_id & 0xFFFFFFFFFFFFFFFF:016X}\n')
    for ev, crc, ranges in func_ids:
        out_lines.append(f'F {ev:08X} {crc & 0xFFFFFFFF:08X}\n')
        for lo, length in ranges:
            out_lines.append(
                f'R {(lo & 0x1FFFFFFF) | 0x80000000:08X} {length:X}\n')
    return ''.join(out_lines)


def overlay_pair_id(src: str, func_ids: list,
                    provenance: str | None = None) -> int:
    """Bind a newly compiled DLL to the exact C and range manifest it uses."""
    digest = hashlib.sha256()
    digest.update(b'psxrecomp overlay pair v1\0')
    digest.update(src.encode('utf-8'))
    digest.update(b'\0')
    digest.update(overlay_ranges_text(
        func_ids, provenance=provenance).encode('ascii'))
    return int.from_bytes(digest.digest()[:8], 'big')


def add_overlay_pair_export(src: str, pair_id: int) -> str:
    """Add the optional v2-pair binding export without changing the shard ABI."""
    return src + f'''\n
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
uint64_t overlay_pair_id(void) {{ return UINT64_C(0x{pair_id:016X}); }}
'''


def write_overlay_ranges_from(func_ids: list, out_path: str,
                              pair_id: int | None = None,
                              provenance: str | None = None) -> int:
    """Write the {phys}_{key}.ranges manifest (v2) from a func-id list produced by
    parse_overlay_func_ids. Returns the number of functions written.

    Manifest v2 line format:
      F <entry_hex> <code_crc_hex>     one per function
      R <lo_hex> <len_hex>             one per coalesced code range"""
    with open(out_path, 'w') as f:
        f.write(overlay_ranges_text(func_ids, pair_id, provenance))
    return len(func_ids)


def write_overlay_ranges(src_path: str, out_path: str,
                         data: bytes, load_addr: int, size: int) -> int:
    """Back-compat wrapper: parse the recompiler manifest and write the v2 .ranges.
    See parse_overlay_func_ids / write_overlay_ranges_from."""
    return write_overlay_ranges_from(
        parse_overlay_func_ids(src_path, data, load_addr, size), out_path)


def load_region_coverage(cache_dir: str, phys_addr: int,
                         expected_abi: int | None = None) -> set:
    """Set of (ev, code_crc) function identities already provided by built DLLs
    for this region_start. The loader content-matches per function across ALL
    DLLs sharing a region_start, so a function is "covered" as soon as ANY
    existing .ranges for that region_start lists it with a matching code_crc.
    overlay-cache v2 uses this to skip the (expensive) gcc compile when a capture
    would add no new function identity (the volatile-data redundant-build case)."""
    covered = set()
    prefix = f'{phys_addr:08X}_'
    try:
        names = os.listdir(cache_dir)
    except OSError:
        return covered
    for name in names:
        if not (name.startswith(prefix) and name.endswith('.ranges')):
            continue
        dll_path = os.path.join(
            cache_dir, os.path.splitext(name)[0] + overlay_ext())
        covered.update(
            (entry, code_crc)
            for entry, code_crc, _ranges in load_shard_func_ids(
                dll_path, expected_abi))
    return covered


def _addr_in_func_ids(addr: int, func_ids: list) -> bool:
    """True if addr falls inside any code range of the given func-id list."""
    a = addr & 0x1FFFFFFF
    for _ev, _crc, ranges in func_ids:
        for lo, length in ranges:
            lo &= 0x1FFFFFFF
            if lo <= a < lo + length:
                return True
    return False


def load_region_entry_set(cache_dir: str, phys_addr: int,
                          expected_abi: int | None = None) -> set:
    """Set of phys-normalized F-line ENTRY addresses provided by ALL built DLLs
    (region + fragment) for this region_start. This — not range coverage — is
    the dispatchability test: native code is enterable ONLY at F entries, so a
    dispatch-proven PC inside a compiled range but absent from every manifest's
    F set still runs its whole chain on the interpreter (the 0x80106D7C class,
    2026-07-06: 80% of Tomba2 attract interp residue was two range-covered,
    entry-less interior PCs that the range-based orphan test refused to
    fragment, while the region compile skipped as 'already covered')."""
    return {
        entry & 0x1FFFFFFF
        for entry, _crc in load_region_coverage(
            cache_dir, phys_addr, expected_abi)
    }


def parse_runtime_shard_manifest(manifest: str,
                                 require_pair: bool = True
                                 ) -> tuple[int | None, list]:
    """Parse only identities representable by the runtime manifest contract."""
    def hex_field(token: str, max_digits: int) -> int:
        if not re.fullmatch(rf'[0-9A-Fa-f]{{1,{max_digits}}}', token):
            raise ValueError(f'invalid manifest hex field: {token!r}')
        return int(token, 16)

    pair_ids = []
    funcs = []
    current = None
    if '\0' in manifest:
        return None, []
    # Runtime reads physical records with fgets(), so only LF terminates a
    # record. A trailing CR in CRLF is ordinary whitespace; a bare CR between
    # directives is not a second record and must therefore fail identically.
    for line in manifest.split('\n'):
        if len(line) > MANIFEST_PHYSICAL_LINE_MAX:
            return None, []
        cr = line.find('\r')
        record_width = len(line) if cr < 0 else cr
        if record_width > MANIFEST_LINE_MAX:
            return None, []
        stripped = line.strip(MANIFEST_ASCII_WHITESPACE)
        parts = (re.split(r'[ \t\r\v\f]+', stripped)
                 if stripped else [])
        if not parts:
            continue
        if parts[0] == 'P':
            if len(parts) != 2:
                return None, []
            try:
                pair_ids.append(hex_field(parts[1], 16))
            except ValueError:
                return None, []
        elif parts[0] == 'F':
            if len(parts) != 3:
                return None, []
            try:
                current = [((hex_field(parts[1], 8) & 0x1FFFFFFF) |
                            0x80000000),
                           hex_field(parts[2], 8), []]
            except ValueError:
                return None, []
            funcs.append(current)
        elif parts[0] == 'R' and current is not None:
            if len(parts) != 3:
                return None, []
            try:
                current[2].append((hex_field(parts[1], 8),
                                   hex_field(parts[2], 8)))
            except ValueError:
                return None, []
    if len(pair_ids) > 1 or (require_pair and len(pair_ids) != 1) or not funcs:
        return None, []
    entries = [entry & 0x1FFFFFFF for entry, _crc, _ranges in funcs]
    if len(set(entries)) != len(entries):
        return None, []
    for entry, _crc, ranges in funcs:
        entry_phys = entry & 0x1FFFFFFF
        if (not 1 <= len(ranges) <= 16 or
                any(length <= 0 for _start, length in ranges) or
                any((start & 0x1FFFFFFF) >= PSX_RAM_SIZE or
                    length > PSX_RAM_SIZE - (start & 0x1FFFFFFF)
                    for start, length in ranges) or
                not any((start & 0x1FFFFFFF) <= entry_phys <
                        (start & 0x1FFFFFFF) + length
                        for start, length in ranges)):
            return None, []
    return (pair_ids[0] if pair_ids else None,
            [tuple(func) for func in funcs])


def load_shard_func_ids(dll_path: str,
                        expected_abi: int | None = None,
                        include_supplemental: bool = True) -> list:
    """Read one runtime-valid shard manifest as ``(entry, crc, ranges)``.

    A nonempty DLL and manifest are not enough: the pair-aware runtime rejects
    a manifest whose ``P`` identity does not match the DLL export.  Discovery
    must apply the same validity rule before allowing a cached shard to
    suppress a needed fragment.
    """
    ranges_path = os.path.splitext(dll_path)[0] + '.ranges'
    try:
        with _shard_pair_lock(dll_path):
            _recover_shard_pair_locked(dll_path)
            if (not os.path.isfile(dll_path) or
                    os.path.getsize(dll_path) == 0 or
                    not os.path.isfile(ranges_path) or
                    os.path.getsize(ranges_path) == 0):
                return []
            with open(ranges_path, encoding='ascii', newline='') as f:
                manifest = f.read()
            if (not include_supplemental and
                    manifest_declares_supplemental_provenance(manifest)):
                return []
            pair_id, valid_funcs = parse_runtime_shard_manifest(
                manifest, require_pair=False)
            if (not valid_funcs or not _dll_runtime_exports_match(
                    dll_path, expected_abi, pair_id,
                    {entry for entry, _crc, _ranges in valid_funcs})):
                return []
    except (OSError, UnicodeError):
        return []
    return valid_funcs


def load_shard_entry_set(dll_path: str,
                         expected_abi: int | None = None) -> set:
    """Phys-normalized F entries from one runtime-valid shard pair."""
    return {
        entry & 0x1FFFFFFF
        for entry, _crc, _ranges in load_shard_func_ids(dll_path, expected_abi)
    }


def load_shard_code_ranges(dll_path: str,
                           expected_abi: int | None = None) -> list:
    """Phys-normalized guarded R intervals from one runtime-valid shard pair."""
    return [
        (start & 0x1FFFFFFF, (start & 0x1FFFFFFF) + length)
        for _entry, _crc, ranges in load_shard_func_ids(dll_path, expected_abi)
        for start, length in ranges if length > 0
    ]


def load_region_current_variant_coverage(cache_dir: str, phys_addr: int,
                                         data: bytes, load_addr: int,
                                         size: int,
                                         expected_abi: int | None = None
                                         ) -> tuple[set, list]:
    """Return exact entries/ranges whose manifest CRC matches ``data``.

    Shards for different byte variants share a region-start namespace.  An F
    entry from another variant cannot satisfy the current capture: the runtime
    will reject it by code CRC.  Recompute each cached function identity over
    the current bytes and expose only identities the runtime can actually call.
    """
    entries = set()
    ranges_out = []
    prefix = f'{phys_addr:08X}_'
    try:
        names = os.listdir(cache_dir)
    except OSError:
        return entries, ranges_out
    for name in names:
        if not (name.startswith(prefix) and name.endswith('.ranges')):
            continue
        dll_path = os.path.join(
            cache_dir, os.path.splitext(name)[0] + overlay_ext())
        shard_entries, shard_ranges = current_variant_func_id_coverage(
            load_shard_func_ids(dll_path, expected_abi), data, load_addr, size)
        entries.update(shard_entries)
        ranges_out.extend(shard_ranges)
    return entries, ranges_out


def current_variant_func_ids(func_ids: list, data: bytes,
                             load_addr: int, size: int) -> list:
    """Retain complete identities whose guarded bytes match this image."""
    valid_ids = []
    ov_lo = load_addr & 0x1FFFFFFF
    ov_hi = ov_lo + size
    for entry, expected_crc, ranges in func_ids:
        crc = 0
        valid = True
        for start, length in ranges:
            lo = start & 0x1FFFFFFF
            if length <= 0 or lo < ov_lo or lo + length > ov_hi:
                valid = False
                break
            crc = binascii.crc32(data[lo - ov_lo:lo - ov_lo + length], crc)
        if valid and (crc & 0xFFFFFFFF) == (expected_crc & 0xFFFFFFFF):
            candidate = [(entry, expected_crc, ranges)]
            if not audit_func_id_delay_slots(candidate, data, load_addr):
                valid_ids.append((entry, expected_crc, ranges))
    return valid_ids


def load_region_current_variant_func_ids(cache_dir: str, phys_addr: int,
                                         data: bytes, load_addr: int,
                                         size: int,
                                         expected_abi: int | None = None,
                                         include_supplemental: bool = True
                                         ) -> list:
    """Complete runtime-valid identities matching one captured byte recipe."""
    out = []
    prefix = f'{phys_addr:08X}_'
    try:
        names = os.listdir(cache_dir)
    except OSError:
        return out
    for name in names:
        if not (name.startswith(prefix) and name.endswith('.ranges')):
            continue
        dll_path = os.path.join(
            cache_dir, os.path.splitext(name)[0] + overlay_ext())
        out.extend(current_variant_func_ids(
            load_shard_func_ids(
                dll_path, expected_abi,
                include_supplemental=include_supplemental),
            data, load_addr, size))
    return out


def current_variant_func_id_coverage(func_ids: list, data: bytes,
                                     load_addr: int,
                                     size: int) -> tuple[set, list]:
    """Filter parsed identities to those callable for these captured bytes."""
    entries = set()
    ranges_out = []
    for entry, _expected_crc, ranges in current_variant_func_ids(
            func_ids, data, load_addr, size):
        entries.add(entry & 0x1FFFFFFF)
        ranges_out.extend(
            ((start & 0x1FFFFFFF), (start & 0x1FFFFFFF) + length)
            for start, length in ranges)
    return entries, ranges_out


# ---------------------------------------------------------------------------
# Doomed-interior fail memo
# ---------------------------------------------------------------------------
# An executed orphan interior whose bytes (in THIS captured image) are data, not
# code, fails the generated-C audit EVERY time — but the fragment pass re-attempts
# it on every autocompile cycle, making the recompiler walk thousands of words of
# data and emit tens of thousands of lines of C only to reject it (measured on
# Vigilante 8: 91 interiors, ~16k walked words each, per cycle). The memo records
# which (region_phys, interior_pc, region_crc) fragments have already failed so
# later cycles SKIP them instead of re-walking. Correctness is preserved:
#   - The key includes region_crc, so a DIFFERENT captured variant (different
#     bytes at the same PC) is NOT memoized — it re-attempts and can succeed.
#   - The memo file lives inside the cg<N>_<hash> cache dir, so ANY codegen/
#     contract/header change (which bumps the hash -> fresh dir -> fresh memo)
#     re-attempts everything (e.g. the psx_rfe_mark_escape contract fix).
# So a memoized skip only ever elides a build that is deterministically doomed.
INTERIOR_FAIL_MEMO = 'interior_fail_memo.txt'


def _interior_fail_key(phys_addr: int, interior: int, data: bytes,
                       load_addr: int, size: int, producer_ranges,
                       cross_call_allow, expected_abi: int, cps: bool,
                       toml_doc: dict) -> str:
    """Stable identity of every input to deterministic fragment generation."""
    recipe = {
        'schema': 'psxrecomp fragment fail key v2',
        'phys_addr': phys_addr,
        'interior': interior & 0x1FFFFFFF,
        'load_addr': load_addr,
        'size': size,
        'data_sha256': hashlib.sha256(data).hexdigest(),
        'producer_ranges': [list(pair) for pair in producer_ranges],
        'cross_call_allow': sorted(set(cross_call_allow)),
        'overlay_abi': expected_abi,
        'cps': bool(cps),
        'toml': toml_doc,
    }
    digest = hashlib.sha256(json.dumps(
        recipe, sort_keys=True, separators=(',', ':'),
        default=str).encode('utf-8')).hexdigest()[:24]
    return f'{phys_addr:08X}_{interior & 0x1FFFFFFF:08X}_{digest}'


def interior_failure_is_deterministic(reason: str) -> bool:
    """Only guest-byte/codegen audit verdicts belong in the persistent memo."""
    return reason.startswith(('generated-c-audit:', 'requested-entry-audit:'))


def optional_static_fragment_rejection(entry: int, job: dict,
                                       reason: str) -> bool:
    """Whether an unplayed speculative candidate failed safely.

    Publishing nothing is the correct outcome for a static-only candidate that
    fails a deterministic byte/codegen audit.  Execution evidence or an
    explicit operator demand turns the same rejection into a real coverage
    failure and therefore remains fatal.
    """
    return (entry in job.get('static_demands', set()) and
            entry not in job.get('executed', set()) and
            entry not in job.get('forced', set()) and
            interior_failure_is_deterministic(reason))


def interior_fail_memo_action(entry: int, job: dict, force: bool) -> str:
    """Choose ``retry``, ``fail``, or ``skip`` for a deterministic miss.

    Required executed/forced demands remain loud without repeating an
    expensive deterministic data-as-code walk. ``--force`` explicitly retries
    it; an unplayed speculative demand remains a safe skip.
    """
    if force:
        return 'retry'
    if (entry in job.get('executed', set()) or
            entry in job.get('forced', set())):
        return 'fail'
    return 'skip'


def load_interior_fail_memo(cache_dir: str) -> dict:
    memo = {}
    try:
        with open(os.path.join(cache_dir, INTERIOR_FAIL_MEMO)) as f:
            for ln in f:
                ln = ln.strip()
                if ln and not ln.startswith('#'):
                    key, _sep, reason = ln.partition('#')
                    memo[key.strip()] = (reason.strip() or
                                         'known deterministic failure')
    except OSError:
        pass
    return memo


def append_interior_fail_memo(cache_dir: str, key: str, reason: str) -> None:
    try:
        os.makedirs(cache_dir, exist_ok=True)
        with open(os.path.join(cache_dir, INTERIOR_FAIL_MEMO), 'a') as f:
            f.write(f'{key}  # {(reason or "").strip()[:120]}\n')
    except OSError:
        pass   # best-effort; a memo write failure must never break the compile


def generate_interior_fragment_static(interior: int, data: bytes,
                                      load_addr: int, size: int,
                                      phys_addr: int, args):
    """Generate one isolated, exact-range-gated static interior shard."""
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 'frag.psx')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(load_addr, interior, data))
        seeds_path = os.path.join(tmp, 'seeds.txt')
        with open(seeds_path, 'w') as f:
            f.write(f'dispatch_root 0x{interior:08X}\n')
        out_dir_tmp = os.path.join(tmp, 'out')
        os.makedirs(out_dir_tmp)
        cmd = [args.recompiler, psx, '--seeds', seeds_path,
               '--out-dir', out_dir_tmp, '--overlay',
               '--ws-config', os.path.abspath(args.game_toml)]
        sub_env = dict(os.environ)
        if args.cps:
            sub_env['PSX_CPS'] = '1'
        result = subprocess.run(
            cmd, capture_output=True, text=True,
            cwd=os.path.dirname(os.path.abspath(args.game_toml)), env=sub_env)
        if result.returncode != 0:
            return None

        full_c = ranges_src = None
        for filename in os.listdir(out_dir_tmp):
            if filename.endswith('_full.c'):
                full_c = os.path.join(out_dir_tmp, filename)
            elif filename.endswith('_full.ranges'):
                ranges_src = os.path.join(out_dir_tmp, filename)
        if not full_c or not ranges_src:
            return None

        with open(full_c) as f:
            src, func_addrs = patch_generated_c_static(
                f.read(), load_addr, size)
        image_crc = binascii.crc32(data) & 0xFFFFFFFF
        audit = audit_generated_c(src, load_addr, size, image_crc, {})
        if audit['unknown_bad'] or audit['unsupported_todo_addrs']:
            return None
        func_ids = parse_overlay_func_ids(ranges_src, data, load_addr, size)
        ids_by_addr = {}
        for ev, code_crc, ranges in func_ids:
            ids_by_addr.setdefault(ev, []).append((code_crc, ranges))
        if set(func_addrs) - set(ids_by_addr):
            return None

        entry = (interior & 0x1FFFFFFF) | 0x80000000
        if entry not in ids_by_addr or entry not in set(func_addrs):
            return None
        namespace = (f'ov_frag_{phys_addr:08X}_{image_crc:08X}_'
                     f'{entry:08X}')
        continuation_owners = parse_cps_continuation_owners(src)
        src, symbols = namespace_generated_static(src, namespace, func_addrs)
        variants = []
        for ev in sorted(func_addrs):
            for code_crc, ranges in ids_by_addr[ev]:
                variants.append({
                    'addr': ev,
                    'symbol': symbols[ev],
                    'crc': code_crc,
                    'ranges': ranges,
                })
        return {
            'src': src,
            'variants': variants,
            'namespace': namespace,
            'func_addrs': set(func_addrs),
            'symbols': symbols,
            'ids_by_addr': ids_by_addr,
            'continuation_owners': continuation_owners,
        }


def validate_overlay_func_ids(func_ids: list,
                              label: str = 'shard') -> str | None:
    """Validate identities against the runtime's fixed manifest contract."""
    entries = [entry & 0x1FFFFFFF for entry, _crc, _ranges in func_ids]
    if len(set(entries)) != len(entries):
        return f'{label} contains duplicate function entries'
    for entry, _crc, ranges in func_ids:
        if not 1 <= len(ranges) <= 16:
            return (f'{label} entry 0x{entry:08X} has {len(ranges)} ranges '
                    '(runtime supports 1..16)')
        if any(length <= 0 for _lo, length in ranges):
            return f'{label} entry 0x{entry:08X} has an empty guarded range'
        if not _addr_in_func_ids(entry, [(entry, 0, ranges)]):
            return f'{label} entry 0x{entry:08X} is outside its guarded ranges'
    return None


def validate_fragment_requested_ids(requested: set[int], frag_ids: list,
                                    generated_defs: set[int]) -> str | None:
    """Return a fail-closed reason when a fragment omitted a requested entry.

    A non-empty manifest is not sufficient: the recompiler can emit some other
    reachable function while rejecting/demoting an exact requested PC.  The
    loader can also retain at most 16 ranges for one function.  Refuse to
    publish unless every requested entry has one unambiguous, runtime-
    representable identity whose guarded bytes include the entry itself.
    """
    for requested_entry in sorted(requested):
        entry = (requested_entry & 0x1FFFFFFF) | 0x80000000
        if entry not in generated_defs:
            return f'requested entry 0x{entry:08X} missing from generated C'
        matches = [
            (ev, crc, ranges) for ev, crc, ranges in frag_ids if ev == entry
        ]
        if len(matches) != 1:
            return (f'requested entry 0x{entry:08X} has {len(matches)} '
                    'manifest identities (expected exactly 1)')
    contract_error = validate_overlay_func_ids(frag_ids, 'fragment')
    if contract_error:
        return contract_error
    for requested_entry in sorted(requested):
        entry = (requested_entry & 0x1FFFFFFF) | 0x80000000
        matches = [
            (ev, crc, ranges) for ev, crc, ranges in frag_ids if ev == entry
        ]
        if not _addr_in_func_ids(entry, matches):
            return f'requested entry 0x{entry:08X} is outside its guarded ranges'
    return None


def validate_interior_fragment_ids(interior: int, frag_ids: list,
                                   generated_defs: set[int]) -> str | None:
    """Compatibility wrapper for a singleton isolated fragment."""
    return validate_fragment_requested_ids(
        {interior}, frag_ids, generated_defs)


def validate_hosted_fragment_ids(requested: set[int], host_specs: dict,
                                 frag_ids: list,
                                 generated_defs: set[int],
                                 allow_missing_targets: bool = False
                                 ) -> str | None:
    """Require every emitted host/alias to equal its selected cached owner.

    The recompiler's host-range block boundary is only a syntactic landing-pad
    check.  Safety comes from this publication gate: the original host and
    every added alias must reproduce the already runtime-validated owner CRC
    and complete guarded-range geometry exactly.
    """
    targets = {_canonical_guest_addr(entry) for entry in requested}
    specs = {
        _canonical_guest_addr(entry): spec
        for entry, spec in host_specs.items()
    }
    if set(specs) != targets:
        return 'hosted target/spec set mismatch'
    expected_by_entry = {}
    hosts = set()
    for target, spec in sorted(specs.items()):
        host = _canonical_guest_addr(spec['host'])
        expected = _normalized_func_identity(spec['owner_identity'])
        if expected[0] != host:
            return f'hosted target 0x{target:08X} owner entry differs from host'
        if target == host:
            return f'hosted target 0x{target:08X} equals its host'
        hosts.add(host)
        prior = expected_by_entry.setdefault(host, expected[1:])
        if prior != expected[1:]:
            return f'hosted host 0x{host:08X} has conflicting identities'
        expected_by_entry[target] = expected[1:]
    if targets & hosts:
        return 'hosted target is also a host in the same shard'
    required = targets | hosts
    contract_error = validate_overlay_func_ids(frag_ids, 'hosted fragment')
    if contract_error:
        return contract_error
    if len(frag_ids) > HOSTED_EMITTED_IDENTITY_CAP:
        return (f'hosted shard emitted {len(frag_ids)} identities '
                f'(cap {HOSTED_EMITTED_IDENTITY_CAP})')
    intervals = sorted(
        (_canonical_guest_addr(start),
         _canonical_guest_addr(start) + length)
        for _entry, _crc, ranges in frag_ids
        for start, length in ranges)
    guarded_bytes = 0
    merged_end = None
    for start, end in intervals:
        if merged_end is None or start > merged_end:
            guarded_bytes += end - start
            merged_end = end
        elif end > merged_end:
            guarded_bytes += end - merged_end
            merged_end = end
    if guarded_bytes > HOSTED_UNIQUE_GUARDED_BYTE_CAP:
        return (f'hosted shard guards {guarded_bytes} unique bytes '
                f'(cap {HOSTED_UNIQUE_GUARDED_BYTE_CAP})')
    by_entry = {}
    for func_id in frag_ids:
        identity = _normalized_func_identity(func_id)
        by_entry.setdefault(identity[0], []).append(identity)
    for entry in sorted(required):
        matches = by_entry.get(entry, [])
        emitted = entry in generated_defs
        if (allow_missing_targets and entry in targets and
                not matches and not emitted):
            continue
        if not emitted:
            return f'requested entry 0x{entry:08X} missing from generated C'
        if len(matches) != 1:
            return (f'hosted entry 0x{entry:08X} has {len(matches)} '
                    'normalized identities (expected exactly 1)')
        if matches[0][1:] != expected_by_entry[entry]:
            return (f'hosted entry 0x{entry:08X} changed selected owner '
                    'CRC/range identity')
    return None


def hosted_fragment_served_targets(requested: set[int], frag_ids: list,
                                   generated_defs: set[int]) -> set[int]:
    """Targets actually emitted after the C++ block-leader proof."""
    manifest_entries = {
        _canonical_guest_addr(entry) for entry, _crc, _ranges in frag_ids
    }
    return ({_canonical_guest_addr(entry) for entry in requested} &
            manifest_entries &
            {_canonical_guest_addr(entry) for entry in generated_defs})


def make_interior_fragment_job(phys_addr: int, load_addr: int, size: int,
                               data: bytes, seed_audit: dict,
                               forced_interiors: set[int],
                               capture: dict | None = None) -> dict | None:
    """Build one explicit fragment job without mutating the shared seed set."""
    interiors = {
        a for a, reason in seed_audit['included_reasons'].items()
        if reason == 'DISPATCH_INTERIOR'
    }
    dispatch_roots = {
        a for a, reason in seed_audit['included_reasons'].items()
        if reason in ('DISPATCH_ENTRY', 'STATIC_DISPATCH_ENTRY',
                      'DISPATCH_ROOT')
    }
    executed = set(seed_audit.get('executed_pcs', set()))
    static_exact_demands = set(
        seed_audit.get('static_exact_fragment_demands', set()))
    hosted_donor_demands = set(
        seed_audit.get('cross_variant_hosted_demands', set()))
    static_interval_demands = set(
        seed_audit.get('static_interval_fragment_demands', set()))
    static_demands = static_exact_demands | static_interval_demands
    region_hi = phys_addr + size
    forced = {
        a for a in forced_interiors
        if phys_addr <= (a & 0x1FFFFFFF) < region_hi
    }
    if not (((interiors or dispatch_roots) and executed) or
            static_demands or forced):
        return None
    return {
        'phys_addr': phys_addr,
        'load_addr': load_addr,
        'size': size,
        'data': data,
        'candidates': interiors | dispatch_roots | static_demands | forced,
        'executed': executed,
        'static_demands': static_demands,
        'static_exact_demands': static_exact_demands,
        'hosted_donor_demands': hosted_donor_demands,
        'included_reasons': {
            addr: {reason}
            for addr, reason in seed_audit['included_reasons'].items()
        },
        'static_interval_demands': static_interval_demands,
        'forced': forced,
        'producer_ranges': tuple(seed_audit['producer_ranges']),
        'cross_call_allow': tuple(seed_audit['accepted_cross_producer_calls']),
        'resident_cap': ({
            'producer': BIOS_RESIDENT_PRODUCER,
            'bios_sha256': capture.get('bios_sha256', ''),
            'producer_name': capture.get('producer_name', 'BIOS resident code'),
        } if capture and capture.get('producer') == BIOS_RESIDENT_PRODUCER
            else None),
    }


def merge_fragment_jobs_by_recipe(jobs: list[dict],
                                  invocation_recipe=()) -> list[dict]:
    """Union evidence that can safely share one byte-variant fragment recipe.

    Repeated captures of identical bytes commonly carry different observed or
    static roots.  Batching each capture independently would still create many
    supplemental DLLs, so merge only jobs whose complete recompiler recipe is
    identical.  Invocation-wide ABI/flavor/CPS/TOML inputs are constant and are
    therefore intentionally not repeated in this key.
    """
    grouped = {}
    set_fields = (
        'candidates', 'executed', 'static_demands',
        'static_exact_demands', 'hosted_donor_demands',
        'static_interval_demands', 'forced',
    )
    for job in jobs:
        key = (
            job['phys_addr'], hashlib.sha256(job['data']).digest(),
            job['load_addr'], job['size'],
            tuple(sorted(job['producer_ranges'])),
            tuple(sorted(set(job['cross_call_allow']))),
            json.dumps(job.get('resident_cap'), sort_keys=True),
            tuple(invocation_recipe),
        )
        merged = grouped.get(key)
        if merged is None:
            merged = dict(job)
            for field in set_fields:
                merged[field] = set(job[field])
            merged['included_reasons'] = {
                addr: set(reasons)
                for addr, reasons in job.get('included_reasons', {}).items()
            }
            grouped[key] = merged
            continue
        for field in set_fields:
            merged[field].update(job[field])
        for addr, reasons in job.get('included_reasons', {}).items():
            merged.setdefault('included_reasons', {}).setdefault(
                addr, set()).update(reasons)
        if merged.get('resident_cap') is None and job.get('resident_cap'):
            merged['resident_cap'] = job['resident_cap']
    return [grouped[key] for key in sorted(grouped)]


def _canonical_guest_addr(addr: int) -> int:
    return (addr & 0x1FFFFFFF) | 0x80000000


def _normalized_func_identity(func_id) -> tuple:
    entry, code_crc, ranges = func_id
    return (_canonical_guest_addr(entry), code_crc & 0xFFFFFFFF,
            tuple((_canonical_guest_addr(start), length)
                  for start, length in ranges))


def _producer_for_job_addr(job: dict, addr: int):
    """Return a stable producer token, treating a simple image as one producer."""
    phys = addr & 0x1FFFFFFF
    ranges = job.get('producer_ranges', ())
    if not ranges:
        lo = job['load_addr'] & 0x1FFFFFFF
        hi = lo + job['size']
        return ('image', lo, hi) if lo <= phys < hi else None
    for index, (start, end) in enumerate(ranges):
        lo = start & 0x1FFFFFFF
        hi = lo + (end - start)
        if lo <= phys < hi:
            return ('producer', index, lo, hi)
    return None


def _preferred_host_reason(reasons) -> str | None:
    """Choose one existing marker deterministically without widening trust."""
    priority = (
        'DIRECT_JAL_TARGET', 'STATIC_INDIRECT_TARGET',
        'STATIC_BRANCH_ROOT', 'STATIC_DISCOVERY_ROOT',
        'STATIC_DISPATCH_ENTRY', 'DISPATCH_ENTRY', 'FUNCTION_POINTER_TARGET',
        'TOML_DECLARED_ENTRY',
    )
    reason_set = set(reasons) & HOSTED_OWNER_REASONS
    return next((reason for reason in priority if reason in reason_set), None)


def select_hosted_interior_demands(job: dict, donor_recipes: dict,
                                   current_func_ids: list,
                                   audit_out: dict | None = None) -> dict:
    """Map sibling-proven targets to unique CRC-valid local host identities.

    This is deliberately fail-closed.  Aliases and multi-range functions are
    not eligible owners: an owner must have one exact range beginning at its F
    entry, carry local root evidence, share the target's producer, and be the
    sole distinct identity containing that target.
    """
    donor_items = sorted(donor_recipes.items())
    nomination_dropped = max(0, len(donor_items) - HOSTED_NOMINATION_CAP)
    donor_items = donor_items[:HOSTED_NOMINATION_CAP]
    lo = job['load_addr'] & 0x1FFFFFFF
    hi = lo + job['size']
    local_reasons = {
        _canonical_guest_addr(addr): set(reasons)
        for addr, reasons in job.get('included_reasons', {}).items()
    }
    identities = {
        _normalized_func_identity(func_id)
        for func_id in current_func_ids
    }
    rejected = Counter()
    owner_intervals = []
    for identity in identities:
        host, _crc, ranges = identity
        reason = _preferred_host_reason(local_reasons.get(host, ()))
        if reason is None:
            rejected['owner_identity_no_local_root'] += 1
            continue
        if len(ranges) != 1:
            rejected['owner_identity_alias_or_multirange'] += 1
            continue
        range_start, length = ranges[0]
        if range_start != host or length <= 0:
            rejected['owner_identity_not_rooted_range'] += 1
            continue
        producer = _producer_for_job_addr(job, host)
        if producer is None:
            rejected['owner_identity_no_producer'] += 1
            continue
        range_end = range_start + length
        if (range_end & 0x1FFFFFFF) > producer[-1]:
            rejected['owner_identity_crosses_producer_end'] += 1
            continue
        owner_intervals.append(
            (range_start, range_end, producer, identity, reason))
    owner_intervals.sort(key=lambda item: (item[0], item[1], item[3]))

    selected = {}
    active_intervals = []
    next_interval = 0
    for raw_target, donors in donor_items:
        target = _canonical_guest_addr(raw_target)
        if not donors:
            rejected['target_no_donor_recipe'] += 1
            continue
        target_phys = target & 0x1FFFFFFF
        if (target & 3) != 0 or not (lo <= target_phys < hi):
            rejected['target_outside'] += 1
            continue
        target_producer = _producer_for_job_addr(job, target)
        if target_producer is None:
            rejected['target_no_producer'] += 1
            continue
        while (next_interval < len(owner_intervals) and
               owner_intervals[next_interval][0] < target):
            active_intervals.append(owner_intervals[next_interval])
            next_interval += 1
        active_intervals = [
            interval for interval in active_intervals
            if interval[1] > target
        ]
        candidates = [
            (identity, reason)
            for _start, _end, producer, identity, reason in active_intervals
            if producer == target_producer
        ]
        # Multiple byte/geometry owners make runtime load order semantically
        # relevant.  Duplicate copies of the identical identity collapse in
        # the set above; any remaining ambiguity is rejected.
        if len(candidates) != 1:
            rejected['target_no_unique_owner'] += 1
            continue
        owner_identity, host_reason = candidates[0]
        selected[target] = {
            'host': owner_identity[0],
            'owner_identity': owner_identity,
            'host_reason': host_reason,
        }
    bounded = {}
    selected_hosts = set()
    for target, spec in sorted(selected.items()):
        host = _canonical_guest_addr(spec['host'])
        if len(bounded) >= HOSTED_SELECTED_ALIAS_CAP:
            rejected['selected_alias_cap_dropped'] += 1
            continue
        if host not in selected_hosts and len(
                selected_hosts) >= HOSTED_SELECTED_HOST_CAP:
            rejected['selected_host_cap_dropped'] += 1
            continue
        bounded[target] = spec
        selected_hosts.add(host)
    selected = bounded
    if nomination_dropped:
        rejected['nomination_cap_dropped'] += nomination_dropped
    if audit_out is not None:
        audit_out.clear()
        audit_out.update(rejected)
        audit_out['donor_targets'] = len(donor_recipes)
        audit_out['current_identities'] = len(identities)
        audit_out['selected'] = len(selected)
    return selected


def organic_root_entries(func_ids) -> set[int]:
    """Return entries whose identity is exactly one range rooted at entry.

    Hosted aliases reuse their owner's range, so their F entry must never gain
    sibling-donor authority merely because the alias survived CRC validation.
    Requiring the same geometry as hosted-owner selection also rejects a later
    island in a multi-range identity from becoming a recursive donor.
    """
    roots = set()
    for func_id in func_ids:
        entry, _crc, ranges = _normalized_func_identity(func_id)
        if len(ranges) != 1:
            continue
        start, length = ranges[0]
        if start == entry and length > 0:
            roots.add(entry)
    return roots


def recipient_sibling_proofs(donor_entries_by_recipe: dict[int, tuple],
                             recipient_index: int,
                             authority_phys_entries: set,
                             limit: int = HOSTED_NOMINATION_CAP
                             ) -> tuple[dict, bool]:
    """Return the fixed first entries proven by another byte recipe.

    Each recipe's donor list is sorted once.  A heap merge excludes the
    recipient before applying the nomination limit. Authority coverage is also
    removed before the cap because it is reconstructed deterministically on
    every invocation. Supplemental coverage deliberately remains in the
    returned window through *all* later selection caps (alias and host as well
    as nomination); ``still_needed`` removes it only at compile time. Filtering
    it here would let an unchanged rerun page into a host that was dropped by a
    later cap. Duplicate addresses collapse.
    """
    streams = [
        entries for index, entries in sorted(donor_entries_by_recipe.items())
        if index != recipient_index and entries
    ]
    if not streams or limit <= 0:
        return {}, bool(streams)
    nominated = []
    previous = None
    have_previous = False
    for entry in heapq.merge(*streams):
        if have_previous and entry == previous:
            continue
        previous = entry
        have_previous = True
        if (entry & 0x1FFFFFFF) in authority_phys_entries:
            continue
        if len(nominated) >= limit:
            return ({entry: True for entry in nominated}, True)
        nominated.append(entry)
    return ({entry: True for entry in nominated}, False)


def select_fragment_orphans(candidates: set[int], executed: set[int],
                            forced: set[int], static_exact: set[int],
                            static_interval: set[int],
                            current_variant_entries: set[int],
                            current_variant_ranges: list) -> list[int]:
    """Select exact demands that the relevant current artifacts cannot serve."""
    def current_region_contains(addr):
        phys = addr & 0x1FFFFFFF
        return any(start <= phys < end for start, end in current_variant_ranges)

    return sorted(
            addr for addr in candidates
        # Every exact demand is byte-variant-specific. A same-address F from a
        # different region variant cannot satisfy it because the runtime's CRC
        # guard will reject that identity against the current bytes.
        if (addr in forced or
            (addr in static_exact and
             (addr & 0x1FFFFFFF) not in current_variant_entries) or
            (addr in static_interval and not current_region_contains(addr)) or
             (addr in executed and
              (addr & 0x1FFFFFFF) not in current_variant_entries)))


def select_batchable_strong_roots(static_exact: set[int], executed: set[int],
                                  forced: set[int], static_interval: set[int],
                                  current_variant_entries: set[int]) -> list[int]:
    """Strong exact roots safe to share a supplemental failure domain."""
    batchable, _isolated = partition_strong_root_demands(
        static_exact, executed, forced, static_interval,
        current_variant_entries)
    return batchable


def partition_strong_root_demands(static_exact: set[int], executed: set[int],
                                  forced: set[int], static_interval: set[int],
                                  current_variant_entries: set[int]
                                  ) -> tuple[list[int], list[int]]:
    """Partition every missing exact root without deferring live overlaps."""
    isolated = executed | forced | static_interval
    missing = {
        entry for entry in static_exact
        if (entry & 0x1FFFFFFF) not in current_variant_entries
    }
    return (sorted(missing - isolated), sorted(missing & isolated))


def compile_batched_fragment_roots(entries, compile_one, on_success,
                                   on_singleton_failure,
                                   still_needed=lambda _entry: True,
                                   should_bisect=lambda _status: True,
                                   on_batch_failure=None) -> int:
    """Compile one root batch, bisecting only when the batch fails closed.

    ``compile_one`` returns the normal ``(func_ids, status)`` pair. Successful
    batches stay grouped; a rejected interaction is recursively partitioned so
    one bad root cannot discard good roots. Only singleton failures are exposed
    to memo/accounting policy.
    """
    roots = sorted(entry for entry in set(entries) if still_needed(entry))
    if not roots:
        return 0
    frag_ids, status = compile_one(roots)
    if frag_ids:
        on_success(roots, frag_ids, status)
        return 1
    if len(roots) == 1:
        on_singleton_failure(roots[0], status)
        return 0
    if not should_bisect(status):
        if on_batch_failure is not None:
            on_batch_failure(roots, status)
        else:
            for root in roots:
                on_singleton_failure(root, status)
        return 0
    middle = len(roots) // 2
    return (compile_batched_fragment_roots(
                roots[:middle], compile_one, on_success,
                on_singleton_failure, still_needed, should_bisect,
                on_batch_failure) +
            compile_batched_fragment_roots(
                roots[middle:], compile_one, on_success,
                on_singleton_failure, still_needed, should_bisect,
                on_batch_failure))


def compile_batched_hosted_aliases(hosted: dict, compile_one, on_success,
                                   on_singleton_failure,
                                   still_needed=lambda _entry: True,
                                   should_bisect=lambda _status: True,
                                   on_batch_failure=None,
                                   attempt_cap=HOSTED_COMPILE_ATTEMPT_CAP,
                                   stop_requested=lambda: False) -> int:
    """Compile bounded host groups, splitting hosts before their targets.

    Multiple host roots can repartition one another. Exact identity validation
    makes that interaction fail closed; host-group-first bisection then removes
    the conflicting root without needlessly exploding every target belonging
    to otherwise compatible hosts.
    """
    by_host = {}
    for raw_target, spec in sorted(hosted.items()):
        target = _canonical_guest_addr(raw_target)
        host = _canonical_guest_addr(spec['host'])
        key = (host, _normalized_func_identity(spec['owner_identity']),
               spec['host_reason'])
        by_host.setdefault(key, []).append(target)

    # A group larger than the per-shard alias bound is split deterministically;
    # each piece still carries exactly one owner identity.
    groups = []
    for key, targets in sorted(by_host.items()):
        unique_targets = sorted(set(targets))
        for offset in range(0, len(unique_targets), HOSTED_BATCH_ALIAS_CAP):
            groups.append((key, unique_targets[
                offset:offset + HOSTED_BATCH_ALIAS_CAP]))

    chunks = []
    chunk = []
    alias_count = 0
    for group in groups:
        group_aliases = len(group[1])
        if chunk and (len(chunk) >= HOSTED_BATCH_HOST_CAP or
                      alias_count + group_aliases > HOSTED_BATCH_ALIAS_CAP or
                      chunk[-1][0][0] == group[0][0]):
            chunks.append(chunk)
            chunk = []
            alias_count = 0
        chunk.append(group)
        alias_count += group_aliases
    if chunk:
        chunks.append(chunk)

    attempts = 0

    def run(group_batch):
        nonlocal attempts
        if stop_requested():
            return 0
        filtered = []
        for key, targets in group_batch:
            needed = [entry for entry in targets if still_needed(entry)]
            if needed:
                filtered.append((key, needed))
        if not filtered:
            return 0
        specs = {}
        for (host, owner_identity, host_reason), targets in filtered:
            for target in targets:
                specs[target] = {
                    'host': host,
                    'owner_identity': owner_identity,
                    'host_reason': host_reason,
                }
        entries = sorted(specs)
        if attempts >= attempt_cap:
            status = (f'hosted-entry-audit: compile attempt cap '
                      f'{attempt_cap} reached')
            if on_batch_failure is not None:
                on_batch_failure(entries, status)
            return 0
        attempts += 1
        frag_ids, status = compile_one(specs)
        if frag_ids:
            served = on_success(entries, frag_ids, status)
            if served is None:
                return 1
            served = ({_canonical_guest_addr(entry) for entry in served} &
                      set(entries))
            if not served:
                status = ('hosted-entry-audit: successful batch reported no '
                          'requested targets')
                if on_batch_failure is not None:
                    on_batch_failure(entries, status)
                else:
                    for entry in entries:
                        on_singleton_failure(entry, status)
                return 1
            missing_groups = []
            for key, targets in filtered:
                missing = [entry for entry in targets if entry not in served]
                if missing:
                    missing_groups.append((key, missing))
            if not missing_groups:
                return 1
            # The recompiler may prove only a subset of a multi-target hosted
            # request. Retry the deterministic remainder now; otherwise a later
            # invocation would shrink the batch and accidentally page into new
            # successes. Every successful retry removes at least one target and
            # the global attempt cap still bounds adversarial inputs.
            return 1 + run(missing_groups)
        if len(filtered) > 1 and should_bisect(status):
            middle = len(filtered) // 2
            return run(filtered[:middle]) + run(filtered[middle:])
        key, targets = filtered[0]
        if len(targets) > 1 and should_bisect(status):
            middle = len(targets) // 2
            return (run([(key, targets[:middle])]) +
                    run([(key, targets[middle:])]))
        if len(targets) == 1:
            on_singleton_failure(targets[0], status)
        elif on_batch_failure is not None:
            on_batch_failure(targets, status)
        else:
            for target in targets:
                on_singleton_failure(target, status)
        return 0

    built = 0
    for chunk_groups in chunks:
        if stop_requested():
            break
        built += run(chunk_groups)
    return built


def hosted_entries_still_missing(hosted: dict, current_phys_entries,
                                 rejected_entries=()) -> list[int]:
    """Return nominated aliases that still need capacity accounting."""
    covered = {int(entry) & 0x1FFFFFFF for entry in current_phys_entries}
    rejected = {_canonical_guest_addr(entry) for entry in rejected_entries}
    return sorted({
        _canonical_guest_addr(entry)
        for entry in hosted
        if ((int(entry) & 0x1FFFFFFF) not in covered and
            _canonical_guest_addr(entry) not in rejected)
    })


def fragment_capacity_allows_entry(saturated: bool, repair_all: bool,
                                   forced_entries, entry: int) -> bool:
    """Whether capacity policy permits one fragment compile attempt.

    A saturated namespace cannot admit ordinary growth.  ``--force`` still
    has to reach the transactional publisher because replacing the same shard
    can be capacity-neutral.  ``--force-interior`` grants that exception only
    to the explicitly named entry.
    """
    return (not saturated or repair_all or
            _canonical_guest_addr(entry) in {
                _canonical_guest_addr(forced)
                for forced in forced_entries
            })


def fragment_capacity_fastpath_eligible(saturated: bool, repair_all: bool,
                                        jobs) -> bool:
    """Whether an already-full invocation may bypass fragment owner analysis."""
    return (saturated and not repair_all and
            not any(job.get('forced') for job in jobs))


def fragment_batch_failure_is_partitionable(reason: str) -> bool:
    """Whether retrying smaller root sets can change this failure verdict."""
    if reason.startswith('candidate-capacity: full'):
        return False
    return reason.startswith((
        'generated-c-audit:', 'requested-entry-audit:',
        'hosted-entry-audit:',
        'candidate-capacity:',
        'no-generated-output', 'no-func-ids',
        'fragment cache-key collision',
    ))


def compile_fragment_batch(requested_entries, data: bytes, load_addr: int,
                           size: int, phys_addr: int, cache_dir: str,
                           args, sub_env: dict, toml_doc: dict,
                           producer_ranges=(), cross_call_allow=(),
                           hosted_owners: dict | None = None,
                           manifest_provenance: str | None = None):
    """Compile an ISOLATED interior-entry 'island' fragment that ENTERS at an
    executed orphan DISPATCH_INTERIOR PC (a host that static analysis never
    discovered, e.g. an FMV driver reached via a computed jump) and covers the
    recompiler's worklist-discovered reachable CFG from that PC — a single
    `dispatch_root` seed; out-of-island branches become dispatcher exits.

    Emitted as its OWN <region>_<key>.dll so a fragment that fails the
    generated-C audit is dropped ALONE and never poisons the region's trusted DLL
    (separate failure domain — the invariant the earlier host-recovery attempt
    violated). Safe because: dispatch_root is exempt from the boundary re-check
    that would reject a mid-function start; the recompiler translates mid-function
    code literally (regs from CPU state, no synthesized stack frame — the caller
    already set up the frame); and the loader's per-function live-RAM CRC gate
    rejects any fragment whose bytes don't match. Returns (frag_ids, status):
    status 'built' or 'cached' on success (frag_ids is the func-id list), or
    (None, reason) on failure/skip so the caller can tally WHY a fragment
    dropped instead of the old silent None."""
    requested = sorted({
        (entry & 0x1FFFFFFF) | 0x80000000
        for entry in requested_entries
    })
    if not requested:
        return None, 'no-requested-entries'
    if hosted_owners is not None:
        hosted_specs = {
            _canonical_guest_addr(entry): spec
            for entry, spec in hosted_owners.items()
            if _canonical_guest_addr(entry) in requested
        }
        if set(hosted_specs) != set(requested):
            return None, 'hosted-entry-audit: target/spec set mismatch'
        hosts = sorted({_canonical_guest_addr(spec['host'])
                        for spec in hosted_specs.values()})
        if set(hosts) & set(requested):
            return None, 'hosted-entry-audit: target is also a host'
        first_entry = hosts[0]
    else:
        hosted_specs = None
        first_entry = requested[0]
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, 'frag.psx')
        with open(psx, 'wb') as f:
            f.write(make_psxexe(load_addr, first_entry, data))
        seeds_path = os.path.join(tmp, 'seeds.txt')
        with open(seeds_path, 'w') as f:
            for range_lo, range_hi in producer_ranges:
                f.write(f'producer_range 0x{range_lo:08X} 0x{range_hi:08X}\n')
            for target in sorted(set(cross_call_allow)):
                f.write(f'cross_call_allow 0x{target:08X}\n')
            if hosted_specs is None:
                for entry in requested:
                    f.write(f'dispatch_root 0x{entry:08X}\n')
            else:
                host_markers = {}
                for entry, spec in sorted(hosted_specs.items()):
                    host = _canonical_guest_addr(spec['host'])
                    marker = seed_line_for_reason(host, spec['host_reason'])
                    prior = host_markers.setdefault(host, marker)
                    if prior != marker:
                        return None, (f'hosted-entry-audit: host '
                                      f'0x{host:08X} has conflicting markers')
                for host, marker in sorted(host_markers.items()):
                    f.write(marker + '\n')
                for entry in requested:
                    host = _canonical_guest_addr(hosted_specs[entry]['host'])
                    f.write(f'hosted_interior 0x{entry:08X} '
                            f'0x{host:08X}\n')
        out_dir_tmp = os.path.join(tmp, 'out')
        os.makedirs(out_dir_tmp)
        cmd = [args.recompiler, psx, '--seeds', seeds_path,
               '--out-dir', out_dir_tmp, '--overlay',
               '--ws-config', os.path.abspath(args.game_toml)]
        r = subprocess.run(cmd, capture_output=True, text=True,
                           cwd=os.path.dirname(os.path.abspath(args.game_toml)),
                           env=sub_env)
        if r.returncode != 0:
            return None, f'recompiler-error: {(r.stderr or r.stdout or "").strip()}'
        full_c = ranges_src = None
        for fn in os.listdir(out_dir_tmp):
            if fn.endswith('_full.c'):
                full_c = os.path.join(out_dir_tmp, fn)
            elif fn.endswith('_full.ranges'):
                ranges_src = os.path.join(out_dir_tmp, fn)
        if not full_c or not ranges_src:
            return None, 'no-generated-output (_full.c/_full.ranges missing)'
        with open(full_c) as f:
            src = patch_generated_c(f.read(), load_addr, size)
        c_audit = audit_generated_c(src, load_addr, size,
                                    binascii.crc32(data) & 0xFFFFFFFF, toml_doc)
        if c_audit['unknown_bad'] or c_audit['unsupported_todo_addrs']:
            # RETAIN the failed fragment's C for triage. Region shards already
            # keep their _patched.c on audit failure; interior fragments used to
            # return here BEFORE the (success-only) retain below, so the exact
            # shards you most want to inspect — WHY did this orphan interior fail
            # to lower — vanished with the temp dir. Write it out with a header
            # summarizing the audit verdict so a failure is drillable, not just
            # counted. (compile_overlays.py is not in the codegen hash: no reshard.)
            try:
                os.makedirs(cache_dir, exist_ok=True)
                ub = ', '.join(f'0x{a:08X}' for a in sorted(c_audit['unknown_bad']))
                us = ', '.join(f'0x{a:08X}'
                               for a in sorted(c_audit['unsupported_todo_addrs']))
                header = (
                    f'/* FRAGMENT AUDIT FAILED — retained for triage (not compiled).\n'
                    f' * requested ({len(requested)}): '
                    f'{", ".join(f"0x{entry:08X}" for entry in requested)}\n'
                    f' * region phys    : 0x{phys_addr:08X}\n'
                    f' * unknown_bad ({len(c_audit["unknown_bad"])}): {ub}\n'
                    f' * unsupported ({len(c_audit["unsupported_todo_addrs"])}): {us}\n'
                    f' */\n')
                request_key = binascii.crc32(b''.join(
                    struct.pack('<I', entry) for entry in requested)) & 0xFFFFFFFF
                failed_c = os.path.join(
                    cache_dir,
                    f'{phys_addr:08X}_{request_key:08X}_fragment_FAILED.c')
                with open(failed_c, 'w') as f:
                    f.write(header + src)
            except OSError:
                pass   # retention is best-effort; never mask the real failure
            return None, (f'generated-c-audit: '
                          f'{len(c_audit["unknown_bad"])} unknown_bad, '
                          f'{len(c_audit["unsupported_todo_addrs"])} unsupported')
        frag_ids = parse_overlay_func_ids(ranges_src, data, load_addr, size)
        if not frag_ids:
            return None, 'no-func-ids (empty ranges manifest)'
        delay_errors = audit_func_id_delay_slots(frag_ids, data, load_addr)
        if delay_errors:
            entry, pc, reason = delay_errors[0]
            where = f' at 0x{pc:08X}' if pc is not None else ''
            return None, (f'delay-slot-identity: func 0x{entry:08X}'
                          f'{where}: {reason}')
        if hosted_specs is None:
            identity_error = validate_fragment_requested_ids(
                set(requested), frag_ids, c_audit['defs'])
            identity_error_prefix = 'requested-entry-audit'
        else:
            identity_error = validate_hosted_fragment_ids(
                set(requested), hosted_specs, frag_ids, c_audit['defs'],
                allow_missing_targets=True)
            identity_error_prefix = 'hosted-entry-audit'
        if identity_error:
            return None, f'{identity_error_prefix}: {identity_error}'
        if hosted_specs is not None:
            served = hosted_fragment_served_targets(
                set(requested), frag_ids, c_audit['defs'])
            if not served:
                return None, ('hosted-no-target: no requested target passed '
                              'the recipient block-leader proof')
        if hosted_specs is not None:
            if manifest_provenance not in (
                    None, HOSTED_MANIFEST_PROVENANCE):
                return None, ('hosted-entry-audit: conflicting manifest '
                              'provenance')
            manifest_provenance = HOSTED_MANIFEST_PROVENANCE
        pair_id = overlay_pair_id(src, frag_ids, manifest_provenance)
        src = add_overlay_pair_export(src, pair_id)
        # Key the fragment DLL by its func-identity SET (dedup like a region
        # bundle); the loader keys DLLs by the region_start filename prefix and
        # content-matches each function, so a fragment is just another DLL for
        # this region_start.
        key = fragment_shard_key(frag_ids, manifest_provenance)
        dll_path = os.path.join(cache_dir, f'{phys_addr:08X}_{key:08X}{overlay_ext()}')
        if not args.force:
            cache_status = cached_shard_manifest_status(
                dll_path, overlay_abi_tag(args.runtime_include, args.flavor),
                overlay_ranges_text(
                    frag_ids, pair_id, manifest_provenance), pair_id)
            if cache_status == 'match':
                return frag_ids, 'cached'   # exact identity already built
            if cache_status == 'mismatch':
                return None, ('fragment cache-key collision/stale pair: existing '
                              'manifest does not match generated identity')
        patched_c = os.path.join(tmp, 'frag_patched.c')
        with open(patched_c, 'w') as f:
            f.write(src)
        # Keep the exact generated fragment beside other retained overlay
        # sources. Orphan interiors are the hardest shards to audit when a
        # native/interpreter differential finds a timing or device mismatch;
        # deleting their only C representation with the temp directory made
        # the responsible lowering impossible to inspect after compilation.
        retained_c = os.path.join(cache_dir, f'{key:08X}_fragment_patched.c')
        with open(retained_c, 'w') as f:
            f.write(src)
        include_dirs = [args.runtime_include]
        recomp_root = os.path.dirname(os.path.dirname(args.recompiler))
        p = os.path.join(recomp_root, 'lib/fmt/include')
        if os.path.isdir(p):
            include_dirs.append(p)
        # Honor the SAME compiler the region path uses (args.compiler / args.tcc).
        # Previously this hardcoded gcc, so on a tcc-only player box (no gcc on
        # PATH) EVERY interior-fragment shard silently failed to build — the
        # FMV-driver / orphan-interior class ran interpreted forever. The cache
        # dir already namespaces by args.compiler, so only the invocation was wrong.
        publication = {}
        if not compile_dll(patched_c, dll_path, include_dirs,
                           gcc=args.gcc, flavor=args.flavor,
                           compiler=args.compiler, tcc=args.tcc,
                           func_ids=frag_ids, pair_id=pair_id,
                           preserve_existing_pair=not args.force,
                           expected_existing_abi=overlay_abi_tag(
                               args.runtime_include, args.flavor),
                           publication_result=publication,
                           candidate_cap=overlay_candidate_cap(
                               args.runtime_include),
                           manifest_provenance=manifest_provenance):
            if publication.get('capacity_error'):
                capacity_error = publication['capacity_error']
                match = re.search(
                    r'(\d+) existing -> \d+ projected .* > (\d+);',
                    capacity_error)
                if match and int(match.group(1)) >= int(match.group(2)):
                    return None, 'candidate-capacity: full; ' + capacity_error
                return None, 'candidate-capacity: ' + capacity_error
            return None, 'compile-error (see COMPILE ERROR above)'
        if not publication.get('published', True):
            # Another process won the content-key publication race. It is a
            # cache hit only when the complete pair is byte-for-byte the same
            # manifest identity; a 32-bit filename-key collision must fail
            # closed instead of warming coverage from our discarded staging.
            cache_status = cached_shard_manifest_status(
                dll_path, overlay_abi_tag(args.runtime_include, args.flavor),
                overlay_ranges_text(
                    frag_ids, pair_id, manifest_provenance), pair_id)
            if cache_status != 'match':
                return None, ('fragment cache-key collision/concurrent pair: '
                              'preserved manifest does not match generated identity')
            return frag_ids, 'cached'
        return frag_ids, 'built'


def compile_interior_fragment(interior: int, data: bytes, load_addr: int,
                              size: int, phys_addr: int, cache_dir: str,
                              args, sub_env: dict, toml_doc: dict,
                              producer_ranges=(), cross_call_allow=()):
    """Compile one isolated uncertain interior entry.

    Executed, operator-forced, and interval-only aliases deliberately use this
    singleton wrapper so one speculative entry cannot poison trusted roots.
    Play-free strong roots use ``compile_fragment_batch`` instead.
    """
    return compile_fragment_batch(
        {interior}, data, load_addr, size, phys_addr, cache_dir, args, sub_env,
        toml_doc, producer_ranges, cross_call_allow,
        manifest_provenance=ORPHAN_MANIFEST_PROVENANCE)


def fragment_shard_key(func_ids: list,
                       provenance: str | None = None) -> int:
    """Filename identity shared by fragment publication and sidecars."""
    crc = 0
    if provenance is not None:
        crc = binascii.crc32(
            b'psxrecomp fragment provenance\0' +
            provenance.encode('ascii') + b'\0')
    return binascii.crc32(b''.join(
        struct.pack('<II', entry, code_crc)
        for entry, code_crc, _ranges in sorted(func_ids)), crc) & 0xFFFFFFFF


def _toolchain_env(gcc: str):
    """Build an environment that guarantees gcc's toolchain dir is on PATH.

    A mingw gcc invoked by absolute path (as the runtime's cmd.exe autocompile
    spawn does, and as any launch without mingw64/bin on PATH does) fails
    SILENTLY — exit 1, empty stderr — because cc1/as/ld/collect2 and the runtime
    DLLs (libwinpthread-1, libgcc_s_seh-1, libstdc++-6) are resolved via PATH and
    can't be found, so gcc never actually compiles. Auto-detect the toolchain
    bin dir and prepend it. Detection order: $PSX_MINGW_BIN override, the dir of
    the given --gcc path, `which gcc`, then common install locations. Returns
    (env, resolved_gcc_dir_or_None)."""
    import shutil
    env = os.environ.copy()
    cands = []
    if os.environ.get('PSX_MINGW_BIN'):
        cands.append(os.environ['PSX_MINGW_BIN'])
    if ('/' in gcc) or ('\\' in gcc):
        cands.append(os.path.dirname(os.path.abspath(gcc)))
    w = shutil.which(gcc)
    if w:
        cands.append(os.path.dirname(w))
    cands += [r'C:\msys64\mingw64\bin', r'C:\mingw64\bin', '/usr/bin']
    for d in cands:
        if d and (os.path.isfile(os.path.join(d, 'gcc.exe')) or
                  os.path.isfile(os.path.join(d, 'gcc'))):
            # Prepend in the RUNNING INTERPRETER'S path flavor. Under an
            # MSYS-flavored python (os.sep == '/'), PATH is a ':'-separated
            # POSIX list; splicing a Windows 'C:/...' entry in (its drive
            # colon reads as a separator) mangles the whole child PATH, so
            # cc1/as/ld/collect2 lose their DLLs and gcc dies exit-1 with
            # EMPTY stderr — the 2026-07-15 in-game shard-compile root cause
            # (runtime cmd.exe /C python resolved to devkitPro's MSYS python).
            # Convert X:[/\]... -> /x/... before prepending; verified live:
            # POSIX-prepend rc 0, Windows-prepend rc 1.
            pd = d
            if os.sep == '/':
                m = re.match(r'^([A-Za-z]):[\\/](.*)$', d)
                if m:
                    pd = '/' + m.group(1).lower() + '/' + m.group(2).replace('\\', '/')
            env['PATH'] = pd + os.pathsep + env.get('PATH', '')
            return env, d
    return env, None


# ---- TinyCC (tcc) overlay compile — toolchain-free user fallback -----------
# tcc has its own built-in linker (no ld/collect2) and bundles its own headers,
# so it is self-contained. The only friction is that tcc 0.9.27 does not skip a
# UTF-8 BOM, and the recompiler's runtime headers carry one — so we feed tcc a
# BOM-stripped copy of the include dirs (used ONLY for tcc's -I; the codegen hash
# still derives from the real headers, so the cache dir matches the runtime).
_TCC_BOMFREE_INC = {}   # orig include dir -> bom-stripped temp dir (memoized)
_TCC_COMPAT_PREFIX = b'''\
#ifdef __TINYC__
/* TinyCC 0.9.27 has no GCC __builtin_ctz. Keep this shim local to the
 * disposable generated source so runtime headers and the codegen hash stay
 * byte-identical across compilers. */
static inline unsigned psx_tcc_ctz(unsigned value) {
    unsigned bit = 0u;
    while (((value >> bit) & 1u) == 0u) ++bit;
    return bit;
}
#define __builtin_ctz(value) psx_tcc_ctz((unsigned)(value))
#endif
'''

def _bom_free_incdir(d: str) -> str:
    d = os.path.abspath(d)
    if d in _TCC_BOMFREE_INC:
        return _TCC_BOMFREE_INC[d]
    out = tempfile.mkdtemp(prefix='tcc_inc_')
    for name in os.listdir(d):
        if not name.endswith('.h'):
            continue
        with open(os.path.join(d, name), 'rb') as f:
            data = f.read()
        if data[:3] == b'\xef\xbb\xbf':
            data = data[3:]
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
    _TCC_BOMFREE_INC[d] = out
    return out

def _compile_dll_tcc(c_path: str, out_dll: str, include_dirs, flavor: int,
                     tcc: str) -> bool:
    # Strip a UTF-8 BOM off the overlay C itself (tcc 0.9.27 chokes on it) and
    # provide the one GCC builtin used by the shared cycle-model header.
    with open(c_path, 'rb') as f:
        data = f.read()
    if data[:3] == b'\xef\xbb\xbf':
        data = data[3:]
    if not data.startswith(_TCC_COMPAT_PREFIX):
        data = _TCC_COMPAT_PREFIX + data
    with open(c_path, 'wb') as f:
        f.write(data)
    cmd = [tcc, '-shared',
           '-DPSX_OVERLAY_DLL_BUILD',
           # Keep TCC semantically identical to the GCC overlay path. Omitting
           # these left debug_server_cyc_observe unresolved in release shards
           # and disabled guest-cycle charging in every successfully linked
           # TCC shard.
           '-DPSX_NO_DEBUG_TOOLS',
           '-DPSX_ENABLE_BLOCK_CYCLES=1',
           f'-DPSX_OVERLAY_FLAVOR={int(flavor)}',
           native_path(c_path), '-o', native_path(out_dll)]
    for d in include_dirs:
        cmd.append('-I' + native_path(_bom_free_incdir(d)))
    print(f'  compile (tcc): {" ".join(cmd)}')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f'  TCC COMPILE ERROR (exit {r.returncode}):\n{r.stderr or r.stdout}')
        return False
    return True


def _compile_dll_direct(c_path: str, out_dll: str, include_dirs: list[str],
                        gcc: str = 'gcc', flavor: int = 0,
                        compiler: str = 'gcc', tcc: str = 'tcc') -> bool:
    import platform
    # Absolute paths (interpreter flavor) for EVERY path the native compiler
    # touches — see native_path's docstring for the two rules and the
    # 2026-07-15 MSYS-python incident they encode.
    c_path  = native_path(c_path)
    out_dll = native_path(out_dll)
    if compiler == 'tcc':
        return _compile_dll_tcc(c_path, out_dll, include_dirs, flavor, tcc)
    env, tc_dir = _toolchain_env(gcc)
    includes = [f'-I{native_path(d)}' for d in include_dirs]
    # On Windows, DLLs use PE relocations — -fPIC triggers GCC CRT init
    # that conflicts with the host process. Use -shared without -fPIC.
    pic_flag = [] if is_windows() else ['-fPIC']
    cmd = [
        gcc, '-shared', *pic_flag, '-O2',
        '-DPSX_OVERLAY_DLL_BUILD',
        # Overlays mirror the runtime's no-debug-tools build: the emitter guards
        # debug_server_cyc_observe (and friends) behind PSX_NO_DEBUG_TOOLS, and the
        # shipped/native runtime is built without debug tools, so define it here too or
        # the overlay emits calls to symbols the runtime doesn't have (undefined ref).
        '-DPSX_NO_DEBUG_TOOLS',
        # CYCLE MODEL UNIFICATION (Tomba2 logo Timer1 fork): compiled-overlay code
        # MUST charge guest cycles exactly like the dirty-RAM interpreter and the
        # BIOS (both built with PSX_ENABLE_BLOCK_CYCLES=1). Without this flag every
        # psx_advance_cycles() in overlay code is #ifdef'd out, so a function run as
        # a native overlay charges ~0 cycles while the same function run via the
        # interp charges per-instruction -> timer-sensitive code (e.g. a Timer1
        # debounce) reads different values per backend and the game forks.
        '-DPSX_ENABLE_BLOCK_CYCLES=1',
        # Codegen-flavor tag baked into overlay_abi() (base=0). The loader
        # rejects DLLs whose flavor differs from the runtime's, so a widescreen
        # cache and a base cache can never cross-contaminate even if they share
        # a directory (they key by guest-bytes CRC, which is flavor-blind).
        f'-DPSX_OVERLAY_FLAVOR={int(flavor)}',
        c_path,
        '-o', out_dll,
        *includes,
        '-lm',
    ]
    if tc_dir is None:
        print('  WARNING: no gcc toolchain dir found (PSX_MINGW_BIN / --gcc dir / '
              'PATH / common locations); compile will likely fail silently.')
    print(f'  compile: {" ".join(cmd)}')
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        msg = (r.stderr or r.stdout or '').strip()
        if not msg:
            # No compiler output at all = the compiler never really ran
            # (spawn/DLL-load failure: foreign-MSYS runtime mix, bad toolchain
            # dir, or non-native paths). Say so instead of printing nothing —
            # an empty COMPILE ERROR is undiagnosable from the runtime's tail.
            msg = ('(no compiler output — gcc likely failed to launch: '
                   f'toolchain dir={tc_dir!r}, check PATH/DLL mix and that '
                   'all paths above are OS-native)')
        print(f'  COMPILE ERROR (exit {r.returncode}):\n{msg}')
        return False
    return True


def compile_dll(c_path: str, out_dll: str, include_dirs: list[str],
                gcc: str = 'gcc', flavor: int = 0,
                compiler: str = 'gcc', tcc: str = 'tcc',
                func_ids: list | None = None,
                pair_id: int | None = None,
                preserve_existing_pair: bool = False,
                expected_existing_abi: int | None = None,
                publication_result: dict | None = None,
                candidate_cap: int | None = None,
                manifest_provenance: str | None = None) -> bool:
    """Publish a shard only after all of its artifacts are complete.

    GCC and tcc write their output incrementally. If the process is interrupted
    while targeting the final cache name, the next run sees that partial file
    and used to skip it as if it were complete. Compile to a unique sibling.

    When ``func_ids`` is supplied, stage the range manifest too and publish the
    two as one recoverable transaction.  Moving the old DLL out of its loader-
    visible name first means every interruption point contains either a complete
    old pair, a complete new pair, or no loadable pair -- never mixed halves.
    The pair ID additionally lets a pair-aware loader reject the only filesystem
    TOCTOU left: an old manifest parsed immediately before the DLL rename.
    """
    final_out = os.path.abspath(out_dll)
    out_dir = os.path.dirname(final_out)
    os.makedirs(out_dir, exist_ok=True)
    if func_ids is not None and (not func_ids or pair_id is None):
        print('  COMPILE ERROR: paired shard requires non-empty ranges and pair ID')
        return False
    if func_ids is not None and candidate_cap is not None:
        try:
            capacity_error = preflight_shard_candidate_capacity(
                final_out, func_ids, pair_id, manifest_provenance,
                expected_existing_abi, candidate_cap)
        except Exception as exc:
            # A failed optimization must not replace the authoritative locked
            # projection after linking.  Compile normally and let publication
            # either admit or reject the staged pair.
            print(f'  capacity preflight unavailable: {exc}')
            capacity_error = None
        if capacity_error is not None:
            if publication_result is not None:
                publication_result['capacity_error'] = capacity_error
            print(f'  COMPILE SKIP (preflight): {capacity_error}')
            return False
    suffix = os.path.splitext(final_out)[1] or overlay_ext()
    fd, staged = tempfile.mkstemp(
        prefix=f'.{os.path.basename(final_out)}.tmp.', suffix=suffix,
        dir=out_dir)
    os.close(fd)
    os.unlink(staged)  # let the native compiler create its output normally
    staged_ranges = os.path.splitext(staged)[0] + '.ranges'
    try:
        if not _compile_dll_direct(
                c_path, staged, include_dirs, gcc=gcc, flavor=flavor,
                compiler=compiler, tcc=tcc):
            return False
        if not os.path.isfile(staged) or os.path.getsize(staged) == 0:
            print('  COMPILE ERROR: compiler reported success but emitted no DLL')
            return False
        if func_ids is None:
            os.replace(staged, final_out)
            if publication_result is not None:
                publication_result['published'] = True
            return True
        write_overlay_ranges_from(
            func_ids, staged_ranges, pair_id, manifest_provenance)
        if not os.path.isfile(staged_ranges) or os.path.getsize(staged_ranges) == 0:
            print('  COMPILE ERROR: range manifest staging produced no output')
            return False
        try:
            published = publish_shard_pair(
                staged, staged_ranges, final_out,
                preserve_existing=preserve_existing_pair,
                expected_existing_abi=expected_existing_abi,
                candidate_cap=candidate_cap)
            if publication_result is not None:
                publication_result['published'] = published
        except ShardCandidateCapacityError as exc:
            if publication_result is not None:
                publication_result['capacity_error'] = str(exc)
            print(f'  COMPILE SKIP: {exc}')
            return False
        except Exception as exc:
            print(f'  COMPILE ERROR: could not publish DLL/ranges pair: {exc}')
            try:
                recover_shard_pair(final_out)
            except Exception as recover_exc:
                print(f'  COMPILE ERROR: shard-pair recovery also failed: '
                      f'{recover_exc}')
            return False
        return True
    finally:
        # TinyCC emits an export-definition sidecar beside its requested DLL.
        # It inherits the unique staged basename and must not accumulate in the
        # content cache after either success or failure.
        staged_sidecars = [os.path.splitext(staged)[0] + '.def']
        for temporary in [staged, staged_ranges, *staged_sidecars]:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass


def _shard_pair_paths(dll_path: str) -> tuple[str, str, str, str]:
    dll_path = os.path.abspath(dll_path)
    ranges_path = os.path.splitext(dll_path)[0] + '.ranges'
    return (dll_path, ranges_path,
            dll_path + '.pair-lock', dll_path + '.pair-txn.json')


def _unlink_if_present(path: str) -> None:
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def _best_effort_unlink(path: str) -> None:
    """Remove transaction debris when possible (loaded Windows DLLs may refuse)."""
    try:
        os.unlink(path)
    except OSError:
        pass


def _file_sha256(path: str) -> str | None:
    try:
        digest = hashlib.sha256()
        with open(path, 'rb') as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b''):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _write_json_atomic(path: str, value: dict) -> None:
    directory = os.path.dirname(path)
    fd, staged = tempfile.mkstemp(prefix=f'.{os.path.basename(path)}.',
                                  suffix='.tmp', dir=directory)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as out:
            json.dump(value, out, sort_keys=True)
            out.flush()
            os.fsync(out.fileno())
        os.replace(staged, path)
    finally:
        _best_effort_unlink(staged)


@contextmanager
def _exclusive_file_lock(lock_path: str, timeout: float | None = 120.0):
    """Kernel-owned cross-process lock on one permanent byte-range file.

    The lock file is intentionally permanent.  OS byte-range/advisory locks are
    released by the kernel when a process dies; deleting the file would create
    an inode/handle split that permits two simultaneous owners.
    """
    lock_path = os.path.abspath(lock_path)
    os.makedirs(os.path.dirname(lock_path), exist_ok=True)
    lock_file = open(lock_path, 'a+b')
    if os.path.getsize(lock_path) == 0:
        lock_file.write(b'\0')
        lock_file.flush()
    deadline = (time.monotonic() + timeout if timeout is not None else None)
    acquired = False
    try:
        while not acquired:
            try:
                lock_file.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX |
                                fcntl.LOCK_NB)
                acquired = True
            except OSError:
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(
                        f'timed out waiting for shard-pair lock {lock_path}')
                time.sleep(0.025)
        yield
    finally:
        if acquired:
            lock_file.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
        lock_file.close()


@contextmanager
def _shard_pair_lock(dll_path: str, timeout: float = 120.0):
    """Cross-process lock for one canonical DLL/ranges filename pair."""
    _dll, _ranges, lock_path, _journal = _shard_pair_paths(dll_path)
    with _exclusive_file_lock(lock_path, timeout):
        yield


def _candidate_capacity_namespace(dll_path: str) -> tuple[str, list[str]]:
    """Return the shared lock and compiler-tier dirs for one cache namespace.

    Normal caches are ``GAME/compiler/arch/cgN_hash``.  The runtime candidate
    table is process-global, so serialize/count every compiler tier with the
    same GAME/arch/cg leaf.  Tests and hand-built paths fall back to their
    containing directory.
    """
    leaf = os.path.dirname(os.path.abspath(dll_path))
    cg_name = os.path.basename(leaf)
    arch_dir = os.path.dirname(leaf)
    compiler_dir = os.path.dirname(arch_dir)
    game_dir = os.path.dirname(compiler_dir)
    compiler_name = os.path.basename(compiler_dir)
    if (re.fullmatch(r'cg\d+_[0-9A-Fa-f]{8}', cg_name) and
            compiler_name in ('gcc', 'tcc')):
        arch_name = os.path.basename(arch_dir)
        tier_dirs = [
            os.path.join(game_dir, tier, arch_name, cg_name)
            for tier in ('gcc', 'tcc')
        ]
        return (os.path.join(game_dir, '.overlay-candidate-capacity.lock'),
                tier_dirs)
    return (os.path.join(leaf, '.overlay-candidate-capacity.lock'), [leaf])


def cache_candidate_capacity_snapshot(
        dll_path: str, expected_abi: int | None) -> tuple[int, dict]:
    """Read one recovered candidate namespace under its publication lock."""
    capacity_lock, cache_dirs = _candidate_capacity_namespace(dll_path)
    with _exclusive_file_lock(capacity_lock):
        recover_candidate_namespace(cache_dirs)
        return cache_candidate_inventory(cache_dirs, expected_abi)


def cache_candidate_capacity_full(dll_path: str, expected_abi: int | None,
                                  candidate_cap: int) -> tuple[bool, int]:
    """Read one serialized namespace inventory and report a saturated cache."""
    total, counts = cache_candidate_capacity_snapshot(dll_path, expected_abi)
    return (total >= candidate_cap or
            getattr(counts, 'raw_total', sum(counts.values())) >=
            2 * candidate_cap or
            getattr(counts, 'range_link_total', 0) >=
            2 * candidate_cap * 8 or
            getattr(counts, 'file_total', len(counts)) >=
            RUNTIME_CACHE_FILE_CAP), total


def candidate_capacity_saturated_in_tier(total: int, counts: dict,
                                         candidate_cap: int,
                                         cache_dir: str) -> bool:
    """Whether one active tier has reached any physical/runtime capacity."""
    active = os.path.normcase(os.path.abspath(cache_dir))
    saturated = (total >= candidate_cap or
                 getattr(counts, 'raw_total', sum(counts.values())) >=
                 2 * candidate_cap or
                 getattr(counts, 'range_link_total', 0) >=
                 2 * candidate_cap * 8 or
                 getattr(counts, 'file_total', len(counts)) >=
                 RUNTIME_CACHE_FILE_CAP)
    return (saturated and bool(counts) and all(
        os.path.normcase(os.path.abspath(os.path.dirname(path))) == active
        for path in counts
    ))


class CandidateInventory(dict):
    """Per-selected-path counts plus the metadata needed for safe projection.

    Keep the historical ``path -> F count`` mapping API: several fixed-point
    callers use its keys to decide which compiler tier owns the inventory.  The
    extra fields distinguish the deduplicated process-lifetime candidate cost
    from physical F-row, range-link, and file costs, none of which may dedup.
    """

    def __init__(self):
        super().__init__()
        self.identities = {}
        self.raw_counts = {}
        self.range_link_counts = {}
        self.raw_total = 0
        self.range_link_total = 0
        self.file_total = 0


RUNTIME_CACHE_FILE_CAP = 4096


def _runtime_manifest_range_link_count(funcs: list) -> int:
    """Mirror LazyMan's unique 4 KiB RAM-page links for parsed F records."""
    page_count = PSX_RAM_SIZE // 4096
    total = 0
    for _entry, _crc, ranges in funcs:
        seen_pages = set()
        for start, length in ranges:
            lo = start & 0x1FFFFFFF
            hi = lo + length - 1
            p0, p1 = lo >> 12, hi >> 12
            if p0 >= page_count:
                continue
            p1 = min(p1, page_count - 1)
            seen_pages.update(range(p0, p1 + 1))
        total += len(seen_pages)
    return total


def _normalized_runtime_manifest_identity(manifest: str, pair_id: int | None,
                                          funcs: list):
    """Return a conservative whole-pair identity, or ``None`` if ambiguous.

    Runtime address aliases are normalized, but F and R ordering is retained:
    range order participates in the guarded CRC calculation.  Compiler-owned
    provenance is part of the identity.  Unknown/malformed provenance and
    legacy manifests without P remain unique rather than being guessed equal.
    """
    if pair_id is None:
        return None
    provenance = manifest_provenance(manifest)
    if (manifest_declares_supplemental_provenance(manifest) and
            provenance is None):
        return None
    normalized_funcs = tuple(
        (entry & 0x1FFFFFFF, crc,
         tuple((start & 0x1FFFFFFF, length) for start, length in ranges))
        for entry, crc, ranges in funcs
    )
    return pair_id, provenance, normalized_funcs


def _candidate_group_identity(cache_dir: str, manifest_identity):
    if manifest_identity is None:
        return None
    return os.path.normcase(os.path.abspath(cache_dir)), manifest_identity


_candidate_validation_cache_lock = threading.Lock()
_candidate_abi_cache = {}
_candidate_record_cache = {}
_candidate_rejection_witness_cache = {}


def _candidate_file_fingerprint(path: str):
    """Return the replacement-sensitive identity of one cache artifact.

    Cache publication uses atomic replacement, so a new pair receives a new
    file identity even when its size and timestamps happen to match.  Keep the
    timestamps as defense for in-place test/repair writes.  Callers still scan
    the namespace and perform the authoritative capacity projection every time;
    this cache only avoids repeatedly mapping unchanged DLLs and reparsing their
    unchanged manifests near the candidate ceiling.
    """
    try:
        stat = os.stat(path)
    except OSError:
        return None
    return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns,
            getattr(stat, 'st_ctime_ns', None))


def _candidate_cached_abi_matches(dll: str, expected_abi: int) -> bool:
    path = os.path.normcase(os.path.abspath(dll))
    fingerprint = (
        expected_abi, _dll_abi_matches,
        _candidate_file_fingerprint(dll),
    )
    with _candidate_validation_cache_lock:
        cached = _candidate_abi_cache.get(path)
        if cached is not None and cached[0] == fingerprint:
            return cached[1]
    matched = _dll_abi_matches(dll, expected_abi)
    # Do not memoize across an uncooperative in-place writer. Cooperating cache
    # publishers are already excluded by the namespace lock held by callers.
    if matched and fingerprint[2] == _candidate_file_fingerprint(dll):
        with _candidate_validation_cache_lock:
            _candidate_abi_cache[path] = (fingerprint, matched)
    return matched


def _load_candidate_inventory_record(dll: str,
                                     expected_abi: int | None):
    """Return valid rows, identity, raw F count, and raw range-link count."""
    ranges_path = os.path.splitext(dll)[0] + '.ranges'
    path = os.path.normcase(os.path.abspath(dll))

    def fingerprint():
        return (
            expected_abi, _dll_runtime_exports_match,
            _candidate_file_fingerprint(dll),
            _candidate_file_fingerprint(ranges_path),
        )

    # A positive cached record is immutable under the cooperating publisher's
    # atomic-replacement contract. Double-stat it before reuse; a changed or
    # transaction-hidden artifact falls through to locked recovery below.
    observed = fingerprint()
    with _candidate_validation_cache_lock:
        cached = _candidate_record_cache.get(path)
    if (cached is not None and cached[0] == observed and
            observed == fingerprint()):
        return cached[1]
    try:
        with _shard_pair_lock(dll):
            _recover_shard_pair_locked(dll)
            if (not os.path.isfile(dll) or os.path.getsize(dll) == 0 or
                    not os.path.isfile(ranges_path) or
                    os.path.getsize(ranges_path) == 0):
                return [], None, 0, 0
            observed = fingerprint()
            with _candidate_validation_cache_lock:
                cached = _candidate_record_cache.get(path)
                if cached is not None and cached[0] == observed:
                    return cached[1]
            with open(ranges_path, encoding='ascii', newline='') as source:
                manifest = source.read()
            pair_id, funcs = parse_runtime_shard_manifest(
                manifest, require_pair=False)
            raw_count = len(funcs)
            range_link_count = _runtime_manifest_range_link_count(funcs)
            if (not funcs or not _dll_runtime_exports_match(
                    dll, expected_abi, pair_id,
                    {entry for entry, _crc, _ranges in funcs})):
                record = ([], None, raw_count, range_link_count)
            else:
                identity = _normalized_runtime_manifest_identity(
                    manifest, pair_id, funcs)
                record = (
                    funcs,
                    _candidate_group_identity(os.path.dirname(dll), identity),
                    raw_count, range_link_count)
            stable = observed == fingerprint()
            # Boolean DLL validation currently conflates deterministic export
            # mismatch with transient LoadLibrary failure. Cache successes only
            # so a one-off loader failure cannot suppress a pair indefinitely.
            if stable and record[0]:
                with _candidate_validation_cache_lock:
                    _candidate_record_cache[path] = (observed, record)
            return record
    except (OSError, UnicodeError):
        return [], None, 0, 0


def cache_candidate_inventory(cache_dirs: list[str],
                              expected_abi: int | None) -> tuple[int, dict]:
    """Inventory runtime-selected pairs with conservative whole-pair dedup.

    Only a non-legacy P identity with the exact normalized manifest semantics
    and provenance may share candidate cost, and only inside one compiler-tier
    leaf. Raw manifest rows, per-F page links, and files remain undeduped.
    """
    ext = overlay_ext()
    canonical = re.compile(
        rf'^[0-9A-Fa-f]{{8}}_[0-9A-Fa-f]{{8}}{re.escape(ext)}$')
    counts = CandidateInventory()
    seen_basenames = set()
    ordered_dirs = []
    for path in cache_dirs:
        absolute = os.path.abspath(path)
        if absolute not in ordered_dirs:
            ordered_dirs.append(absolute)
    for cache_dir in ordered_dirs:
        try:
            names = sorted(os.listdir(cache_dir))
        except OSError:
            continue
        for name in names:
            if not canonical.fullmatch(name):
                continue
            dll = os.path.abspath(os.path.join(cache_dir, name))
            if (expected_abi is not None and
                    not _candidate_cached_abi_matches(dll, expected_abi)):
                # Runtime's ABI preflight removes this path from the index
                # before scanning the next compiler tier, so it cannot shadow
                # a valid lower-tier basename.
                continue
            basename = os.path.normcase(name)
            if basename in seen_basenames:
                continue
            # Runtime scans gcc before tcc and suppresses a lower-tier path by
            # basename even when the selected higher-tier pair later fails ABI
            # validation. Mirror that exact candidate population here.
            seen_basenames.add(basename)
            funcs, identity, raw_count, range_link_count = \
                _load_candidate_inventory_record(dll, expected_abi)
            counts[dll] = len(funcs)
            counts.identities[dll] = identity
            counts.raw_counts[dll] = raw_count
            counts.range_link_counts[dll] = range_link_count
    counts.raw_total = sum(counts.raw_counts.values())
    counts.range_link_total = sum(counts.range_link_counts.values())
    counts.file_total = len(counts)
    seen_identities = set()
    total = 0
    for path, count in counts.items():
        identity = counts.identities[path]
        if identity is None or identity not in seen_identities:
            total += count
        if identity is not None:
            seen_identities.add(identity)
    return total, counts


def recover_candidate_namespace(cache_dirs: list[str]) -> None:
    """Recover transaction-hidden canonical pairs before capacity inventory."""
    ext = overlay_ext()
    journal_suffix = ext + '.pair-txn.json'
    canonical = re.compile(
        rf'^[0-9A-Fa-f]{{8}}_[0-9A-Fa-f]{{8}}{re.escape(ext)}$')
    for cache_dir in cache_dirs:
        try:
            journals = sorted(
                name for name in os.listdir(cache_dir)
                if name.endswith(journal_suffix))
        except OSError:
            continue
        for name in journals:
            dll_name = name[:-len('.pair-txn.json')]
            if canonical.fullmatch(dll_name):
                recover_shard_pair(os.path.join(cache_dir, dll_name))


def _projected_cache_capacity_usage(total: int, counts: dict,
                                    cache_dirs: list[str], final_dll: str,
                                    staged_count: int, staged_identity=None,
                                    staged_range_links: int = 0
                                    ) -> tuple[int, int, int, int]:
    """Model candidate, lazy-F, range-link, and file usage."""
    final_dll = os.path.abspath(final_dll)
    final_dir = os.path.normcase(os.path.dirname(final_dll))
    ranks = {
        os.path.normcase(os.path.abspath(path)): index
        for index, path in enumerate(cache_dirs)
    }
    final_rank = ranks.get(final_dir, 0)
    basename = os.path.normcase(os.path.basename(final_dll))
    selected_path = next((
        path for path in counts
        if os.path.normcase(os.path.basename(path)) == basename
    ), None)
    records = [(path, count,
                getattr(counts, 'identities', {}).get(path),
                getattr(counts, 'raw_counts', {}).get(path, count),
                getattr(counts, 'range_link_counts', {}).get(path, 0))
               for path, count in counts.items()]
    if selected_path is not None:
        selected_rank = ranks.get(
            os.path.normcase(os.path.dirname(selected_path)), 0)
        if final_rank > selected_rank:
            # A lower-tier publication is invisible while a same-basename
            # higher tier exists.
            return (total, getattr(counts, 'raw_total', sum(counts.values())),
                    getattr(counts, 'range_link_total', 0),
                    getattr(counts, 'file_total', len(counts)))
        records = [record for record in records if record[0] != selected_path]
    group_identity = _candidate_group_identity(final_dir, staged_identity)
    records.append((final_dll, staged_count, group_identity, staged_count,
                    staged_range_links))

    projected = 0
    seen_identities = set()
    for _path, count, identity, _raw_count, _range_links in records:
        if identity is None or identity not in seen_identities:
            projected += count
        if identity is not None:
            seen_identities.add(identity)
    return (projected,
            sum(raw_count for _path, _count, _identity, raw_count, _links
                in records),
            sum(links for _path, _count, _identity, _raw, links in records),
            len(records))


def projected_cache_candidate_usage(total: int, counts: dict,
                                    cache_dirs: list[str], final_dll: str,
                                    staged_count: int,
                                    staged_identity=None) -> tuple[int, int, int]:
    """Compatibility API for candidate, lazy-F, and file usage."""
    projected, raw, _range_links, files = _projected_cache_capacity_usage(
        total, counts, cache_dirs, final_dll, staged_count, staged_identity)
    return projected, raw, files


def projected_cache_candidate_count(total: int, counts: dict,
                                    cache_dirs: list[str], final_dll: str,
                                    staged_count: int,
                                    staged_identity=None) -> int:
    """Compatibility wrapper returning process-lifetime candidate usage."""
    return projected_cache_candidate_usage(
        total, counts, cache_dirs, final_dll, staged_count,
        staged_identity)[0]


def _candidate_capacity_projection_error(
        total: int, counts: dict, projected: int, projected_raw: int,
        projected_range_links: int, projected_files: int,
        staged_count: int, candidate_cap: int) -> str | None:
    """Return the same fail-closed capacity verdict before or after linking."""
    raw_total = getattr(counts, 'raw_total', sum(counts.values()))
    range_link_total = getattr(counts, 'range_link_total', 0)
    file_total = getattr(counts, 'file_total', len(counts))
    if projected > candidate_cap and projected > total:
        return ('overlay candidate capacity would be exceeded: '
                f'{total} existing -> {projected} projected with '
                f'{staged_count} staged > {candidate_cap}; canonical '
                'pair preserved')
    if projected_raw > 2 * candidate_cap and projected_raw > raw_total:
        return ('overlay lazy-manifest capacity would be exceeded: '
                f'{raw_total} existing -> {projected_raw} projected > '
                f'{2 * candidate_cap}; canonical pair preserved')
    if (projected_range_links > 2 * candidate_cap * 8 and
            projected_range_links > range_link_total):
        return ('overlay lazy range-link capacity would be exceeded: '
                f'{range_link_total} existing -> '
                f'{projected_range_links} projected > '
                f'{2 * candidate_cap * 8}; canonical pair preserved')
    if (projected_files > RUNTIME_CACHE_FILE_CAP and
            projected_files > file_total):
        return ('overlay cache file capacity would be exceeded: '
                f'{file_total} existing -> {projected_files} projected > '
                f'{RUNTIME_CACHE_FILE_CAP}; canonical pair preserved')
    return None


def preflight_shard_candidate_capacity(
        final_dll: str, func_ids: list, pair_id: int,
        manifest_provenance: str | None, expected_abi: int | None,
        candidate_cap: int) -> str | None:
    """Reject an impossible publication before invoking the native linker.

    The locked publication check remains authoritative.  This early check is
    deliberately one-sided: a concurrent deletion after the snapshot can only
    defer safe coverage until the next invocation, while an admitted shard is
    still reprojected under the namespace lock after it has been compiled.
    """
    final_dll = os.path.abspath(final_dll)
    capacity_lock, cache_dirs = _candidate_capacity_namespace(final_dll)
    manifest = overlay_ranges_text(
        func_ids, pair_id, manifest_provenance)
    staged_identity = _normalized_runtime_manifest_identity(
        manifest, pair_id, func_ids)
    staged_range_links = _runtime_manifest_range_link_count(func_ids)
    with _exclusive_file_lock(capacity_lock):
        cache_dirs_key = tuple(
            os.path.normcase(os.path.abspath(path)) for path in cache_dirs)
        witness_key = (
            os.path.normcase(os.path.abspath(capacity_lock)), cache_dirs_key,
            expected_abi, candidate_cap, _dll_abi_matches,
            _dll_runtime_exports_match,
        )
        with _candidate_validation_cache_lock:
            witness = _candidate_rejection_witness_cache.get(witness_key)
        if witness is not None:
            witness_total, witness_counts = witness
            witness_usage = _projected_cache_capacity_usage(
                witness_total, witness_counts, cache_dirs, final_dll,
                len(func_ids), staged_identity, staged_range_links)
            witness_error = _candidate_capacity_projection_error(
                witness_total, witness_counts, *witness_usage,
                len(func_ids), candidate_cap)
            if witness_error is not None:
                # Reuse only a proof of impossibility. Added pairs make the
                # bound stronger; deletion/replacement can merely defer safe
                # optional coverage until the next command. An apparent
                # admission always falls through to a fresh locked inventory.
                return witness_error
        recover_candidate_namespace(cache_dirs)
        total, counts = cache_candidate_inventory(cache_dirs, expected_abi)
        with _candidate_validation_cache_lock:
            _candidate_rejection_witness_cache[witness_key] = (total, counts)
        usage = _projected_cache_capacity_usage(
            total, counts, cache_dirs, final_dll, len(func_ids),
            staged_identity, staged_range_links)
        return _candidate_capacity_projection_error(
            total, counts, *usage, len(func_ids), candidate_cap)


def _cleanup_old_pair_debris(dll_path: str) -> None:
    for path in glob.glob(os.path.abspath(dll_path) + '.pair-txn.*.old-*'):
        _best_effort_unlink(path)


def _recover_shard_pair_locked(dll_path: str) -> None:
    dll, ranges, _lock, journal_path = _shard_pair_paths(dll_path)
    if not os.path.isfile(journal_path):
        _cleanup_old_pair_debris(dll)
        return
    try:
        with open(journal_path, encoding='utf-8') as source:
            txn = json.load(source)
    except (OSError, ValueError) as exc:
        raise RuntimeError(f'cannot recover shard transaction {journal_path}: {exc}')

    required = ('backup_dll', 'backup_ranges', 'new_dll_sha256',
                'new_ranges_sha256', 'old_dll_sha256', 'old_ranges_sha256')
    if txn.get('schema') != 'psxrecomp shard pair transaction v1' or any(
            key not in txn for key in required):
        raise RuntimeError(f'invalid shard transaction journal {journal_path}')

    backup_dll = txn['backup_dll']
    backup_ranges = txn['backup_ranges']
    token = txn.get('token', '')
    if (not re.fullmatch(r'[0-9a-f]{32}', token) or
            backup_dll != f'{dll}.pair-txn.{token}.old-dll' or
            backup_ranges != f'{dll}.pair-txn.{token}.old-ranges'):
        raise RuntimeError(f'invalid shard transaction ownership in {journal_path}')
    new_complete = (_file_sha256(dll) == txn['new_dll_sha256'] and
                    _file_sha256(ranges) == txn['new_ranges_sha256'])
    if new_complete:
        # The DLL rename is the commit point.  A crash before a journal phase
        # update must not roll back a new DLL already paired with its exact
        # manifest.  Remove the journal first; old loaded-DLL backups are merely
        # deferred debris and may remain locked until process exit on Windows.
        _unlink_if_present(journal_path)
        _best_effort_unlink(backup_dll)
        _best_effort_unlink(backup_ranges)
        return

    def restore(path: str, backup: str, expected_hash: str | None) -> None:
        current_hash = _file_sha256(path)
        if expected_hash is None:
            if current_hash is not None:
                _unlink_if_present(path)
            return
        if current_hash == expected_hash:
            return
        if _file_sha256(backup) != expected_hash:
            raise RuntimeError(
                f'cannot recover {path}: neither canonical nor backup is the old file')
        if current_hash is not None:
            _unlink_if_present(path)
        os.replace(backup, path)

    # Restore the manifest first; restoring the DLL is the rollback commit.
    restore(ranges, backup_ranges, txn['old_ranges_sha256'])
    restore(dll, backup_dll, txn['old_dll_sha256'])
    _unlink_if_present(journal_path)
    _best_effort_unlink(backup_dll)
    _best_effort_unlink(backup_ranges)


def recover_shard_pair(dll_path: str) -> None:
    """Recover/finish a dead writer's transaction under its OS-owned lock."""
    with _shard_pair_lock(dll_path):
        _recover_shard_pair_locked(dll_path)


def _runtime_valid_shard_pair_locked(dll: str, ranges: str,
                                     expected_abi: int | None) -> bool:
    """Mirror the pair-aware loader's minimum acceptance checks."""
    if (not os.path.isfile(dll) or os.path.getsize(dll) == 0 or
            not os.path.isfile(ranges) or os.path.getsize(ranges) == 0):
        return False
    try:
        with open(ranges, encoding='ascii', newline='') as f:
            manifest = f.read()
        pair_id, funcs = parse_runtime_shard_manifest(
            manifest, require_pair=False)
    except (OSError, UnicodeError):
        return False
    return (bool(funcs) and _dll_runtime_exports_match(
        dll, expected_abi, pair_id,
        {entry for entry, _crc, _ranges in funcs}))


def publish_shard_pair(staged_dll: str, staged_ranges: str,
                       final_dll: str, preserve_existing: bool = False,
                       expected_existing_abi: int | None = None,
                       candidate_cap: int | None = None) -> bool:
    """Commit a staged pair, optionally preserving a valid concurrent winner.

    Region DLL names are keyed only by whole-region CRC. Two processes can
    compile the same bytes with different discovery evidence; the later,
    poorer writer must not overwrite a richer pair after that richer process
    has used its own result to suppress fragment repair. First valid publisher
    wins, while each process's post-pass re-reads the actual winner and emits
    any exact-demand fragments it still lacks.
    """
    dll, ranges, _lock, journal_path = _shard_pair_paths(final_dll)
    token = uuid.uuid4().hex
    backup_dll = f'{dll}.pair-txn.{token}.old-dll'
    backup_ranges = f'{dll}.pair-txn.{token}.old-ranges'

    capacity_lock, cache_dirs = _candidate_capacity_namespace(dll)
    capacity_guard = (_exclusive_file_lock(capacity_lock)
                      if candidate_cap is not None else nullcontext())
    with capacity_guard:
        projected = None
        staged_count = 0
        total = 0
        if candidate_cap is not None:
            try:
                with open(staged_ranges, encoding='ascii', newline='') as source:
                    staged_manifest = source.read()
                    staged_pair, staged_funcs = (
                        parse_runtime_shard_manifest(staged_manifest))
            except (OSError, UnicodeError) as exc:
                raise RuntimeError(
                    f'cannot read staged range manifest: {exc}') from exc
            if not staged_funcs:
                raise RuntimeError(
                    'staged range manifest has no runtime-valid F records')
            staged_count = len(staged_funcs)
            recover_candidate_namespace(cache_dirs)
            total, counts = cache_candidate_inventory(
                cache_dirs, expected_existing_abi)
            staged_identity = _normalized_runtime_manifest_identity(
                staged_manifest, staged_pair, staged_funcs)
            staged_range_links = _runtime_manifest_range_link_count(staged_funcs)
            (projected, projected_raw, projected_range_links,
             projected_files) = _projected_cache_capacity_usage(
                    total, counts, cache_dirs, dll, staged_count,
                    staged_identity, staged_range_links)
        with _shard_pair_lock(dll):
            _recover_shard_pair_locked(dll)
            if (preserve_existing and _runtime_valid_shard_pair_locked(
                    dll, ranges, expected_existing_abi)):
                return False
            if candidate_cap is not None:
                capacity_error = _candidate_capacity_projection_error(
                    total, counts, projected, projected_raw,
                    projected_range_links, projected_files, staged_count,
                    candidate_cap)
                if capacity_error is not None:
                    raise ShardCandidateCapacityError(capacity_error)
            txn = {
                'schema': 'psxrecomp shard pair transaction v1',
                'token': token,
                'backup_dll': backup_dll,
                'backup_ranges': backup_ranges,
                'old_dll_sha256': _file_sha256(dll),
                'old_ranges_sha256': _file_sha256(ranges),
                'new_dll_sha256': _file_sha256(staged_dll),
                'new_ranges_sha256': _file_sha256(staged_ranges),
            }
            if txn['new_dll_sha256'] is None or txn['new_ranges_sha256'] is None:
                raise RuntimeError('staged DLL/ranges disappeared before publication')
            _write_json_atomic(journal_path, txn)
            try:
                if txn['old_dll_sha256'] is not None:
                    os.replace(dll, backup_dll)
                if txn['old_ranges_sha256'] is not None:
                    os.replace(ranges, backup_ranges)

                # The new manifest alone is inert. Publishing the DLL is the commit.
                os.replace(staged_ranges, ranges)
                os.replace(staged_dll, dll)
                _recover_shard_pair_locked(dll)  # recognizes committed hashes
            except BaseException:
                _recover_shard_pair_locked(dll)
                raise
    return True


def _dll_abi_matches(dll_path: str, expected_abi: int) -> bool:
    """Read overlay_abi from one loaded image, then release it immediately."""
    import _ctypes
    import ctypes
    library = None
    try:
        # Shards are generated as ordinary C (cdecl). ctypes.WinDLL selects
        # stdcall and can corrupt a 32-bit Windows caller's stack; CDLL is the
        # correct loader on every supported host (x64 merely masks the error).
        library = ctypes.CDLL(dll_path)
        abi_fn = library.overlay_abi
        abi_fn.argtypes = []
        abi_fn.restype = ctypes.c_int
        return (abi_fn() & 0xFFFFFFFF) == (expected_abi & 0xFFFFFFFF)
    except (OSError, AttributeError):
        return False
    finally:
        if library is not None and library._handle:
            handle = library._handle
            library._handle = 0
            if os.name == 'nt':
                _ctypes.FreeLibrary(handle)
            else:
                _ctypes.dlclose(handle)


def _dll_pair_id_matches(dll_path: str, expected_pair_id: int) -> bool:
    """Read overlay_pair_id from one loaded image, then release it."""
    import _ctypes
    import ctypes
    library = None
    try:
        library = ctypes.CDLL(dll_path)
        pair_fn = library.overlay_pair_id
        pair_fn.argtypes = []
        pair_fn.restype = ctypes.c_uint64
        return pair_fn() == (expected_pair_id & 0xFFFFFFFFFFFFFFFF)
    except (OSError, AttributeError):
        return False
    finally:
        if library is not None and library._handle:
            handle = library._handle
            library._handle = 0
            if os.name == 'nt':
                _ctypes.FreeLibrary(handle)
            else:
                _ctypes.dlclose(handle)


def _dll_runtime_exports_match(dll_path: str, expected_abi: int | None,
                               expected_pair_id: int | None,
                               entries: set[int]) -> bool:
    """Validate the exports the runtime needs in one library mapping."""
    import _ctypes
    import ctypes
    library = None
    try:
        library = ctypes.CDLL(dll_path)
        abi_fn = library.overlay_abi
        abi_fn.argtypes = []
        abi_fn.restype = ctypes.c_int
        if (expected_abi is not None and
                (abi_fn() & 0xFFFFFFFF) != (expected_abi & 0xFFFFFFFF)):
            return False
        try:
            pair_fn = library.overlay_pair_id
        except AttributeError:
            if expected_pair_id is not None:
                return False
        else:
            if expected_pair_id is None:
                return False
            pair_fn.argtypes = []
            pair_fn.restype = ctypes.c_uint64
            if pair_fn() != (expected_pair_id & 0xFFFFFFFFFFFFFFFF):
                return False
        getattr(library, 'overlay_init')
        getattr(library, 'overlay_flush_cycles')
        for entry in entries:
            virt = (entry & 0x1FFFFFFF) | 0x80000000
            getattr(library, f'func_{virt:08X}')
        return True
    except (OSError, AttributeError):
        return False
    finally:
        if library is not None and library._handle:
            handle = library._handle
            library._handle = 0
            if os.name == 'nt':
                _ctypes.FreeLibrary(handle)
            else:
                _ctypes.dlclose(handle)


def compiled_shard_complete(dll_path: str,
                            expected_abi: int | None = None) -> bool:
    """Whether the pair satisfies the runtime's manifest/export contract."""
    return bool(load_shard_func_ids(dll_path, expected_abi))


def shard_pair_files_complete(dll_path: str) -> bool:
    """Filesystem-only completeness used by crash-transaction tests."""
    ranges = os.path.splitext(dll_path)[0] + '.ranges'
    with _shard_pair_lock(dll_path):
        _recover_shard_pair_locked(dll_path)
        return (os.path.isfile(dll_path) and
                os.path.getsize(dll_path) > 0 and
                os.path.isfile(ranges) and
                os.path.getsize(ranges) > 0)


def cached_shard_manifest_status(dll_path: str, expected_abi: int | None,
                                 expected_manifest: str,
                                 expected_pair_id: int | None = None) -> str:
    """Return ``match``, ``mismatch``, or ``missing`` under the pair lock."""
    ranges = os.path.splitext(dll_path)[0] + '.ranges'
    with _shard_pair_lock(dll_path):
        _recover_shard_pair_locked(dll_path)
        complete = (os.path.isfile(dll_path) and
                    os.path.getsize(dll_path) > 0 and
                    os.path.isfile(ranges) and
                    os.path.getsize(ranges) > 0)
        if not complete:
            return 'missing'
        try:
            with open(ranges) as f:
                actual_manifest = f.read()
        except OSError:
            return 'missing'
        if expected_pair_id is not None:
            if (expected_abi is not None and
                    not _dll_abi_matches(dll_path, expected_abi)):
                # A stale flavor/ABI is replaceable, not an identity collision.
                return 'missing'
            try:
                entries = {
                    int(parts[1], 16)
                    for parts in (line.split()
                                  for line in actual_manifest.splitlines())
                    if len(parts) >= 3 and parts[0] == 'F'
                }
            except ValueError:
                return 'mismatch'
            if (not entries or not _dll_runtime_exports_match(
                    dll_path, None, expected_pair_id, entries)):
                return 'mismatch'
        elif (expected_abi is not None and
              not _dll_abi_matches(dll_path, expected_abi)):
            return 'missing'
        return 'match' if actual_manifest == expected_manifest else 'mismatch'


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--captures',        default=None,
                    help='overlay_captures.json from the runtime. Optional: the '
                         'runtime injects PSX_OVERLAY_CAPTURES (the canonical '
                         '<exe>/overlay_captures.json), which wins. Required only '
                         'for manual/offline invocation.')
    ap.add_argument('--game-toml',       required=True,
                    help='game.toml (for game_id)')
    ap.add_argument('--recompiler',      required=True,
                    help='path to psxrecomp-game.exe')
    ap.add_argument('--runtime-include', required=True,
                    help='path to psxrecomp runtime/include dir')
    ap.add_argument('--out-dir',         default='build-dev/cache',
                    help='cache root dir (default: build-dev/cache)')
    ap.add_argument('--gcc',             default='gcc',
                    help='GCC binary (default: gcc)')
    ap.add_argument('--compiler',        choices=['gcc', 'tcc'], default='gcc',
                    help='overlay shard compiler: gcc (default; dev/production, '
                         'best-optimized) or tcc (bundled, toolchain-free user '
                         'fallback). tcc shards land in the tcc/ cache namespace.')
    ap.add_argument('--tcc',             default='tcc',
                    help='TinyCC binary (used when --compiler tcc)')
    ap.add_argument('--force',           action='store_true',
                    help='recompile even if output already exists')
    ap.add_argument('--only-region', action='append', default=[],
                    help='compile only captures whose normalized load address '
                         'matches this value (repeatable; manual validation aid)')
    ap.add_argument('--check',           action='store_true',
                    help='PREFLIGHT: build every shard into a throwaway temp dir '
                         '(implies --force, ignores any injected cache dir, never '
                         'touches the real cache) and report pass/fail. Answers '
                         '"would this game\'s shards compile against the CURRENT '
                         'recompiler + runtime headers?" — exits non-zero if any '
                         'shard that should build fails. Use after a header/ABI '
                         'or emitter change to catch silent shard breakage.')
    ap.add_argument('--force-interior',  action='append', default=[],
                    help='also compile this virtual/physical PC as an isolated '
                         'interior fragment (repeatable; diagnostic recovery for '
                         'an observed dispatch PC whose classifier provenance was '
                         'lost after a later capture)')
    ap.add_argument('--static',          action='store_true',
                    help='B-2 mode: compile into binary (overlays_static.c) instead of DLL')
    ap.add_argument('--flavor',          type=int, default=0,
                    help='codegen flavor id baked into overlay_abi() (0=base/master; '
                         'widescreen build passes 1). The loader rejects DLLs whose '
                         'flavor != the runtime flavor, keeping caches separate.')
    ap.add_argument('--cps',             action='store_true',
                    help='continuation-passing (RECURSION_BUG.md §25): set PSX_CPS '
                         'when invoking the recompiler so overlay funcs tail-transfer '
                         '+ carry an entry-switch. Must match the runtime build.')
    ap.add_argument('--jobs',            type=int,
                    default=max(1, (os.cpu_count() or 4) - 2),
                    help='parallel region-group workers (default: cores-2). '
                         'Captures are grouped by region start; regions are '
                         'independent (dedup coverage, prior-ranges merge, '
                         'fragments, and filenames all key on the region), '
                         'captures within one region stay ordered. 1 = the '
                         'sequential path. --static always runs sequential.')
    args = ap.parse_args()
    forced_interiors = {
        (int(v, 0) & 0x1FFFFFFF) | 0x80000000
        for v in args.force_interior
    }

    # ---- Framework-injected cache location wins over CLI flags ----------------
    # The runtime (autocompile.c) exports PSX_OVERLAY_CACHE_DIR / PSX_OVERLAY_CAPTURES
    # set to the loader's CANONICAL <exe>/cache and <exe>/overlay_captures.json. We
    # honor those above --out-dir/--captures so the WRITE cache and READ captures
    # are always exactly where the loader reads — no per-game config, no drift
    # between dev and prod (the read/write-location divergence bug). The CLI flags
    # remain the fallback for manual/offline invocation (no env set).
    # --check is a preflight: it must NEVER write the real cache, so it ignores
    # the injected cache dir and builds into a throwaway temp dir with --force.
    _check_tmp = None
    if args.check:
        args.force = True
        _check_tmp = tempfile.mkdtemp(prefix='shardcheck_')
        args.out_dir = _check_tmp
        print(f'[check] preflight build into throwaway dir: {_check_tmp}')
    _env_out = None if args.check else os.environ.get('PSX_OVERLAY_CACHE_DIR')
    if _env_out:
        if _env_out != args.out_dir:
            print(f'[cache] PSX_OVERLAY_CACHE_DIR overrides --out-dir: {_env_out}')
        args.out_dir = _env_out
    _env_cap = os.environ.get('PSX_OVERLAY_CAPTURES')
    if _env_cap:
        if _env_cap != args.captures:
            print(f'[cache] PSX_OVERLAY_CAPTURES overrides --captures: {_env_cap}')
        args.captures = _env_cap
    if not args.captures:
        ap.error('no captures file: set PSX_OVERLAY_CAPTURES (runtime injects it) '
                 'or pass --captures for manual/offline use')

    # Resolve the recompiler exe and runtime-include to absolute paths up front.
    # game.toml's overlay_autocompile_cmd uses paths RELATIVE to the project root
    # (the runtime spawns this script via cmd.exe with cwd = project root) so the
    # shipped config is portable across machines — never bake absolute paths into
    # game.toml. But two Windows quirks break relative paths once we hand them off:
    #   1. subprocess/CreateProcess does NOT resolve a relative *executable*
    #      against the child cwd (the recompiler spawn below, cwd=toml_dir), so a
    #      relative --recompiler fails with WinError 2 and the cache never warms.
    #   2. gcc launched from cmd.exe rejects relative forward-slash -I/-c/-o paths
    #      (already handled inside compile_dll via os.path.abspath).
    # Anchoring both here (against this process's cwd = project root, where the
    # relative paths are meant to resolve) makes relative config work everywhere.
    args.recompiler      = os.path.abspath(args.recompiler)
    args.runtime_include = os.path.abspath(args.runtime_include)

    # Load the shard link-contract preamble from runtime/include up front (hard
    # error if absent). Threads/fragments read the module global thereafter.
    load_dispatch_preamble(args.runtime_include)

    # Read game ID from game.toml (strip BOM if present)
    with open(args.game_toml, 'rb') as f:
        raw = f.read().lstrip(b'\xef\xbb\xbf')  # UTF-8 BOM
    toml = tomllib.loads(raw.decode('utf-8'))
    game_id = toml.get('game', {}).get('id', 'UNKNOWN')
    print(f'Game ID: {game_id}')

    # Stale-recompiler-binary guard — BOTH modes (static overlays are just as
    # wrong when emitted by a stale binary; they simply have no tag to hide in).
    verify_recompiler_matches_tag(args.recompiler, codegen_hash(args.runtime_include))

    if args.static:
        static_out = os.path.join(args.out_dir, 'overlays_static.c')
        if os.path.exists(static_out) and not args.force:
            print(f'SKIP: {static_out} already exists (use --force to recompile)')
            return
        os.makedirs(args.out_dir, exist_ok=True)
    else:
        # Namespaced + versioned gcc cache: <game_id>/gcc/<arch-abi>/cg<N>/
        # (SLJIT.md §4 — no comingling; cg<N> = codegen version so a new emitter
        # build never reuses a stale DLL, old versions coexist). MUST match
        # overlay_loader.c scan_cache_dir(). Pre-1.0: no legacy fallback.
        cg = codegen_ver(args.runtime_include)
        ch = codegen_hash(args.runtime_include)
        cache_dir = os.path.join(args.out_dir, game_id, args.compiler, cache_arch_abi(),
                                 f'cg{cg}_{ch:08x}')
        os.makedirs(cache_dir, exist_ok=True)
        print(f'Cache dir: {cache_dir}  (codegen ver {cg}, hash {ch:08x})')

    with open(args.captures) as f:
        captures = json.load(f)

    if args.only_region:
        only_regions = {int(value, 0) & 0x1FFFFFFF
                        for value in args.only_region}
        captures = [cap for cap in captures
                    if (int(cap['load_addr'], 16) & 0x1FFFFFFF) in only_regions]
        if not captures:
            ap.error('--only-region did not match any capture')

    print(f'Captures: {len(captures)} overlay(s) to process\n')

    # B-2 static mode: accumulate privately-namespaced generated C plus exact
    # per-function identities for the content-validated dispatcher.
    static_parts = []
    static_requested_entries = set()
    static_entry_sources = {}

    # overlay-cache v2: per-region_start function-identity coverage, so a capture
    # that adds no NEW (entry, code_crc) skips the gcc compile (volatile-data
    # redundant-build elimination). Lazily loaded from existing .ranges, kept warm
    # in-memory and updated as we build, so repeats within one run also dedup.
    region_coverage_cache = {}   # phys_addr -> set((ev, code_crc))
    cov_lock = threading.Lock()  # guards region_coverage_cache + its sets
    # Per-region info for the post-loop interior-entry fragment pass (decoupled
    # from region-compile success): (phys, load_addr, size, data, interior_pcs,
    # executed_pcs). Collected right after classification so it survives a region
    # whose own compile is skipped or audit-fails.
    interior_frag_jobs = []

    # Per-capture body, extracted so the region-parallel driver below can call
    # it. All shared state is either read-only closure (args/toml/cache_dir)
    # or passed per-region (region_coverage_cache / interior_frag_jobs), so a
    # worker owning a region owns every mutable it touches.
    def _do_capture(cap, region_coverage_cache, interior_frag_jobs, stats):
        load_addr = int(cap['load_addr'], 16)
        size      = int(cap['size'])
        data      = base64.b64decode(cap['bytes_b64'])
        crc32     = binascii.crc32(data) & 0xFFFFFFFF
        phys_addr = (load_addr & 0x1FFFFFFF)
        _label = f'overlay 0x{load_addr:08X} crc {crc32:08X}'
        if args.static:
            for captured_entry in _parse_addr_list(
                    cap.get('dispatch_entry_pcs', [])):
                entry = ((captured_entry & 0x1FFFFFFF) | 0x80000000)
                static_requested_entries.add(entry)
                static_entry_sources[entry] = (
                    data, load_addr, size, phys_addr)
            # --force-interior is an explicit operator assertion that a live
            # dispatch entry was observed even if the retained capture lost its
            # classifier provenance. Static mode must honor it just like DLL
            # mode: bind the requested PC to this capture's exact bytes, then
            # the post-pass will build a content-validated isolated shard.
            region_hi = phys_addr + size
            for forced_entry in forced_interiors:
                forced_phys = forced_entry & 0x1FFFFFFF
                if phys_addr <= forced_phys < region_hi:
                    entry = forced_phys | 0x80000000
                    static_requested_entries.add(entry)
                    static_entry_sources[entry] = (
                        data, load_addr, size, phys_addr)

        # Reclassify prior F entry addresses from an identical image. Callable
        # entries may become current roots; other entries re-enter as dispatch
        # candidates and require a current exact host. Never reuse prior R host
        # ranges: later roots can repartition the same bytes incompatibly.
        if not args.static:
            ranges_name = f'{phys_addr:08X}_{crc32:08X}.ranges'
            prior_ranges_path = os.path.join(cache_dir, ranges_name)
            if os.path.exists(prior_ranges_path):
                prior_entries = []
                with open(prior_ranges_path) as pf:
                    for ln in pf:
                        parts = ln.split()
                        if parts and parts[0] == 'F':
                            try:
                                prior_entries.append(int(parts[1], 16))
                            except (IndexError, ValueError):
                                pass
                if prior_entries:
                    fe = _parse_addr_list(cap.get('function_entry_pcs', []))
                    de = _parse_addr_list(cap.get('dispatch_entry_pcs', []))
                    for a in prior_entries:
                        if _callable_legacy_seed(data, load_addr, a):
                            fe.add(a)
                        else:
                            de.add(a)
                    cap['function_entry_pcs'] = sorted(fe)
                    cap['dispatch_entry_pcs'] = sorted(de)
                    cap.setdefault('schema', 'merged')
                    print(f'  merged {len(prior_entries)} prior-manifest entries '
                          f'from {prior_ranges_path}')

        seeds, seed_audit = classify_overlay_seeds(cap, data, load_addr, size,
                                                   crc32, toml)
        this_ids = None   # region func-ids once recompiled (None if skipped early)

        # Record this region's executed dispatch-proven PCs for the decoupled
        # fragment pass (runs after the loop, regardless of this region's
        # compile outcome). Interiors AND callable dispatch roots both go in:
        # a callable root can be starved by the DLL-already-exists skip when
        # an OLDER capture of the same image bytes built this region's DLL
        # before the PC became dispatch-proven (same filename, stale entry
        # set — the 0x80024548 class), and the fragment pass is the demand-
        # driven recovery path for exactly that.
        if not args.static:
            # A manual exact demand must not depend on the capture still
            # retaining executed_pcs or on today's classifier assigning the PC
            # DISPATCH_INTERIOR.  That provenance is precisely what
            # --force-interior restores.  Keep it isolated from the shared
            # region seed set and queue the fragment directly.
            fragment_job = make_interior_fragment_job(
                phys_addr, load_addr, size, data, seed_audit,
                forced_interiors, cap)
            if fragment_job is not None:
                interior_frag_jobs.append(fragment_job)

        if not args.static:
            dll_path = os.path.join(cache_dir, f'{phys_addr:08X}_{crc32:08X}{overlay_ext()}')

        print(f'Overlay  load=0x{load_addr:08X}  size={size}  crc32=0x{crc32:08X}')
        if args.static:
            print(f'  seeds: {len(seeds)}  mode: static -> {static_out}')
        else:
            print(f'  seeds: {len(seeds)}  dll: {dll_path}')
        print_seed_audit(seed_audit)

        demanded_root_entries = walk_root_seed_entries(seeds)
        root_seeds = [
            seed for seed in seeds
            if ((seed.split()[0] in ('call_root', 'dispatch_root')) or
                seed.split()[0].startswith('0x'))
        ]
        if not root_seeds:
            print('  SKIP: no walk-root seeds (data-only region)\n')
            stats.add_skip()
            return

        # Cheap evidence-aware cache gate. A valid existing pair is not enough:
        # a later capture can add roots for identical bytes. Skip reanalysis only
        # when runtime-callable identities matching THIS byte variant already
        # serve every current walk-root demand. The fragment job was queued
        # above and still repairs any exact interior/static demand independently.
        if not args.static and not args.force:
            expected_abi = overlay_abi_tag(
                args.runtime_include, args.flavor)
            if cap.get('producer') == BIOS_RESIDENT_PRODUCER:
                # A resident fast-path needs one canonical preloadable bundle;
                # aggregate fragment coverage is insufficient because the
                # sidecar names exactly one DLL stem.
                current_entries, _current_ranges = (
                    current_variant_func_id_coverage(
                        load_shard_func_ids(dll_path, expected_abi),
                        data, load_addr, size))
            else:
                current_entries, _current_ranges = (
                    load_region_current_variant_coverage(
                        cache_dir, phys_addr, data, load_addr, size,
                        expected_abi))
            if demanded_root_entries <= {
                    (entry & 0x1FFFFFFF) | 0x80000000
                    for entry in current_entries}:
                reconcile_bios_resident_marker(dll_path, cap, False)
                print(f'  SKIP: runtime-valid current-byte cache serves all '
                      f'{len(demanded_root_entries)} walk-root demand(s)\n')
                stats.add_skip()
                return

        with tempfile.TemporaryDirectory() as tmp:
            # Write fake PS-EXE. The header entry PC becomes a walk root in the
            # recompiler, so it must be a walk-root seed — never an 'interior'
            # one. Root seed lines are either '0x...' or 'dispatch_root 0x...'.
            entry_pc = int(root_seeds[0].split()[-1], 16)
            psx_path = os.path.join(tmp, f'overlay_{load_addr:08X}.psx')
            with open(psx_path, 'wb') as f:
                f.write(make_psxexe(load_addr, entry_pc, data))

            # Write seeds file
            seeds_path = os.path.join(tmp, 'seeds.txt')
            with open(seeds_path, 'w') as f:
                for s in seeds:
                    f.write(s + '\n')

            out_dir_tmp = os.path.join(tmp, 'out')
            os.makedirs(out_dir_tmp)

            # Run psxrecomp-game in --overlay mode (always, for every overlay
            # input). Evidence-scoped discovery: compile only the proven entry
            # seeds and the code reachable from them; never whole-byte sweep
            # (which decodes embedded data tables as code). Branch/jump-table
            # targets stay as in-parent labels, not standalone functions. This
            # is the overlay-compilation contract, not a tunable.
            cmd = [args.recompiler, psx_path,
                   '--seeds', seeds_path,
                   '--out-dir', out_dir_tmp,
                   '--overlay',
                   # Forward the [widescreen] site lists so overlay-resident
                   # emits (backdrop screenX squash, and any sprite-tag/cull
                   # sites that resolve into overlay code) are applied. --ws-config
                   # only adopts the widescreen lists, not the game's exe/paths.
                   '--ws-config', os.path.abspath(args.game_toml)]
            print(f'  recompile: {args.recompiler} ...{" [CPS]" if args.cps else ""}')
            toml_dir = os.path.dirname(os.path.abspath(args.game_toml))
            sub_env = dict(os.environ)
            if args.cps:
                sub_env['PSX_CPS'] = '1'   # §25: emit continuation-passing overlay C
            r = subprocess.run(cmd, capture_output=True, text=True,
                               cwd=toml_dir, env=sub_env)
            if r.returncode != 0:
                print(f'  RECOMPILER ERROR:\n{r.stderr or r.stdout}')
                stats.add_fail(_label, 'recompiler', r.stderr or r.stdout)
                return

            # Find the generated _full.c
            stem = os.path.basename(psx_path)
            full_c = os.path.join(out_dir_tmp, stem + '_full.c')
            if not os.path.exists(full_c):
                # psxrecomp-game uses filename() not stem()
                candidates = [f for f in os.listdir(out_dir_tmp) if f.endswith('_full.c')]
                if not candidates:
                    print(f'  ERROR: no _full.c in {out_dir_tmp}')
                    stats.add_fail(_label, 'no_output', 'no _full.c emitted')
                    return
                full_c = os.path.join(out_dir_tmp, candidates[0])

            with open(full_c) as f:
                src = f.read()

            ranges_src = None
            for fn in os.listdir(out_dir_tmp):
                if fn.endswith('_full.ranges'):
                    ranges_src = os.path.join(out_dir_tmp, fn)
                    break

            # Post-process
            if args.static:
                src, func_addrs = patch_generated_c_static(src, load_addr, size)
                continuation_owners = parse_cps_continuation_owners(src)
                c_audit = audit_generated_c(src, load_addr, size, crc32, toml)
                print_generated_c_audit(load_addr, size, crc32, c_audit)
                if c_audit['unknown_bad'] or c_audit['unsupported_todo_addrs']:
                    print('  GENERATED-C AUDIT FAILED\n')
                    stats.add_fail(_label, 'audit',
                                   f'{len(c_audit["unknown_bad"])} unknown_bad, '
                                   f'{len(c_audit["unsupported_todo_addrs"])} unsupported')
                    return
                if not ranges_src:
                    print('  STATIC RANGE AUDIT FAILED: no _full.ranges manifest\n')
                    stats.add_fail(_label, 'no_ranges', 'no _full.ranges manifest')
                    return

                func_ids = parse_overlay_func_ids(ranges_src, data,
                                                  load_addr, size)
                ids_by_addr = {}
                for ev, code_crc, ranges in func_ids:
                    ids_by_addr.setdefault(ev, []).append((code_crc, ranges))
                missing = sorted(set(func_addrs) - set(ids_by_addr))
                if missing:
                    sample = ', '.join(f'0x{a:08X}' for a in missing[:8])
                    print(f'  STATIC RANGE AUDIT FAILED: {len(missing)} '
                          f'dispatchable function(s) lack exact ranges: {sample}\n')
                    stats.add_fail(_label, 'static_ranges',
                                   f'{len(missing)} funcs lack exact ranges')
                    return

                # Whole-image identity plus compiled-entry coverage makes the
                # namespace deterministic while allowing a later, richer seed
                # capture of identical bytes to coexist without symbol clashes.
                cov_blob = ','.join(f'{a:08X}'
                                    for a in sorted(func_addrs)).encode()
                cov_crc = binascii.crc32(cov_blob) & 0xFFFFFFFF
                namespace = f'ov_{phys_addr:08X}_{crc32:08X}_{cov_crc:08X}'
                src, symbols = namespace_generated_static(src, namespace,
                                                          func_addrs)
                variants = []
                for ev in sorted(func_addrs):
                    for code_crc, ranges in ids_by_addr[ev]:
                        variants.append({
                            'addr': ev,
                            'symbol': symbols[ev],
                            'crc': code_crc,
                            'ranges': ranges,
                        })
                static_parts.append({
                    'src': src,
                    'variants': variants,
                    'namespace': namespace,
                    'func_addrs': set(func_addrs),
                    'symbols': symbols,
                    'ids_by_addr': ids_by_addr,
                    'continuation_owners': continuation_owners,
                })
                print(f'  recompiled: {len(func_addrs)} functions, '
                      f'{len(variants)} exact identities\n')
                stats.add_ok()
            else:
                src = patch_generated_c(src, load_addr, size)
                c_audit = audit_generated_c(src, load_addr, size, crc32, toml)
                print_generated_c_audit(load_addr, size, crc32, c_audit)
                # Always save the debug copy for inspection — including on audit
                # failure, so opcode gaps / boundary artifacts can be classified.
                os.makedirs(os.path.dirname(dll_path), exist_ok=True)
                debug_c = os.path.join(os.path.dirname(dll_path),
                                       f'{crc32:08X}_patched.c')
                with open(debug_c, 'w') as f:
                    f.write(src)
                if c_audit['unknown_bad'] or c_audit['unsupported_todo_addrs']:
                    fallback = optional_enrichment_fallback_capture(cap)
                    if fallback is not None:
                        print('  OPTIONAL ENRICHMENT AUDIT REJECTED; '
                              'retrying conservative recipe\n')
                        _do_capture(fallback, region_coverage_cache,
                                    interior_frag_jobs, stats)
                        return
                    print('  GENERATED-C AUDIT FAILED\n')
                    stats.add_fail(_label, 'audit',
                                   f'{len(c_audit["unknown_bad"])} unknown_bad, '
                                   f'{len(c_audit["unsupported_todo_addrs"])} unsupported')
                    return
                patched_c = os.path.join(tmp, 'overlay_patched.c')
                with open(patched_c, 'w') as f:
                    f.write(src)

                # overlay-cache v2 dedup: compute this capture's per-function
                # identity set BEFORE the (expensive) gcc compile. The loader
                # content-matches each function by (entry, code_crc) across ALL
                # DLLs at this region_start, so if every function we'd produce is
                # already provided by an existing DLL, a new DLL adds nothing —
                # skip the build. This is what stops volatile-data regions (a
                # changing whole-region CRC over byte-identical code) from minting
                # an endless pile of redundant DLLs.
                this_ids = (parse_overlay_func_ids(ranges_src, data, load_addr, size)
                            if ranges_src else [])
                delay_errors = audit_func_id_delay_slots(
                    this_ids, data, load_addr)
                if delay_errors:
                    entry, pc, reason = delay_errors[0]
                    where = f' at 0x{pc:08X}' if pc is not None else ''
                    print(f'  DELAY-SLOT IDENTITY AUDIT FAILED: '
                          f'func 0x{entry:08X}{where}: {reason}\n')
                    stats.add_fail(_label, 'delay_slot_identity', reason)
                    return
                this_set = {(ev, crc) for ev, crc, _ in this_ids}

                with cov_lock:
                    covered = region_coverage_cache.get(phys_addr)
                    if covered is None:
                        covered = load_region_coverage(
                            cache_dir, phys_addr,
                            overlay_abi_tag(args.runtime_include, args.flavor))
                        region_coverage_cache[phys_addr] = covered
                    fully_covered = (bool(this_set) and this_set <= covered
                                     and not args.force
                                     and cap.get('producer') !=
                                         BIOS_RESIDENT_PRODUCER)
                if fully_covered:
                    print(f'  SKIP: all {len(this_set)} function(s) already '
                          f'covered by existing DLL(s) at this region — no new '
                          f'native code to build\n')
                    stats.add_skip()
                    return

                if not this_ids:
                    print('  WARNING: recompiler emitted no usable _full.ranges -- '
                          'preserving any prior DLL/ranges pair and leaving this '
                          'region to the interpreter')
                    stats.add_fail(_label, 'no_ranges',
                                   'no usable function identities (DLL not built)')
                    return
                missing_exports = {
                    entry for entry, _crc, _ranges in this_ids
                    if entry not in c_audit['defs']
                }
                if missing_exports:
                    detail = ', '.join(
                        f'0x{entry:08X}' for entry in sorted(missing_exports))
                    print('  GENERATED-C/MANIFEST EXPORT AUDIT FAILED: '
                          f'{detail}\n')
                    stats.add_fail(
                        _label, 'manifest_exports',
                        f'{len(missing_exports)} manifest entries lack C definitions')
                    return
                representability_error = validate_overlay_func_ids(
                    this_ids, 'region')
                if representability_error:
                    print('  RANGE MANIFEST AUDIT FAILED: '
                          f'{representability_error}\n')
                    stats.add_fail(
                        _label, 'manifest_ranges', representability_error)
                    return
                pair_id = overlay_pair_id(src, this_ids)
                src = add_overlay_pair_export(src, pair_id)
                # Retained source is the exact source compiled into the DLL.
                with open(patched_c, 'w') as f:
                    f.write(src)

                # Compile to DLL
                include_dirs = [args.runtime_include]
                recomp_root = os.path.dirname(os.path.dirname(args.recompiler))
                for lib_inc in ['lib/fmt/include']:
                    p = os.path.join(recomp_root, lib_inc)
                    if os.path.isdir(p):
                        include_dirs.append(p)

                publication = {}
                success = compile_dll(patched_c, dll_path, include_dirs,
                                      gcc=args.gcc, flavor=args.flavor,
                                      compiler=args.compiler, tcc=args.tcc,
                                      func_ids=this_ids,
                                      pair_id=pair_id,
                                      preserve_existing_pair=(
                                          not args.force and
                                          cap.get('producer') !=
                                          BIOS_RESIDENT_PRODUCER),
                                      expected_existing_abi=overlay_abi_tag(
                                          args.runtime_include, args.flavor),
                                      publication_result=publication,
                                      candidate_cap=overlay_candidate_cap(
                                          args.runtime_include))
                if success:
                    # compile_dll transactionally published the per-entry range
                    # manifest beside the DLL from the same func-id list used by
                    # dedup. The loader keys it by the same filename stem.
                    ranges_out = os.path.splitext(dll_path)[0] + '.ranges'
                    if this_ids:
                        nfn = len(this_ids)
                        published = publication.get('published', True)
                        reconcile_bios_resident_marker(
                            dll_path, cap, published)
                        print(f'  ranges: {nfn} functions -> {ranges_out}')
                        # New identities are now available for this region_start;
                        # keep the warm coverage set current so later captures in
                        # this same run dedup against them. (Parallel note: the
                        # check→build→update window is deliberately unlocked, so
                        # two concurrent captures can both build overlapping DLLs.
                        # That is redundancy, not corruption — the loader content-
                        # matches every function by (entry, code_crc) across all
                        # DLLs at a region.)
                        with cov_lock:
                            covered |= load_region_coverage(
                                cache_dir, phys_addr,
                                overlay_abi_tag(
                                    args.runtime_include, args.flavor))
                        if published:
                            print(f'  OK -> {dll_path}\n')
                            stats.add_ok()
                        else:
                            print(f'  PRESERVED concurrent valid pair -> '
                                  f'{dll_path}\n')
                            stats.add_skip()
                    else:
                        print('  WARNING: recompiler emitted no _full.ranges — '
                              'loader will leave this region to the interpreter')
                        # A DLL with no .ranges is dead weight: the loader has no
                        # per-function identities to dispatch, so the region stays
                        # interpreted. Tally it as a failure, not a silent "OK".
                        stats.add_fail(_label, 'no_ranges',
                                       'DLL built but no _full.ranges (undispatchable)')
                else:
                    if publication.get('capacity_error'):
                        print('  CACHE CAPACITY REACHED; shard left to the '
                              'interpreter\n')
                        stats.add_skip()
                        return
                    fallback = optional_enrichment_fallback_capture(cap)
                    if fallback is not None:
                        print('  OPTIONAL ENRICHMENT COMPILE REJECTED; '
                              'retrying conservative recipe\n')
                        _do_capture(fallback, region_coverage_cache,
                                    interior_frag_jobs, stats)
                        return
                    print(f'  FAILED\n')
                    stats.add_fail(_label, 'compile',
                                   'gcc/tcc compile failed (see COMPILE ERROR above)')

    # Interior-entry "island" fragments (overlay-cache v2): the FMV-driver class.
    # Run as a SEPARATE pass AFTER all region compiles, so it is DECOUPLED from
    # region success — an executed orphan interior gets its isolated island shard
    # even if its region's trusted compile failed/audit-failed (that is the whole
    # point of the separate failure domain). For each region, find DISPATCH_INTERIOR
    # PCs that ACTUALLY EXECUTED this session but that NO built DLL covers (orphan
    # interiors — host never discovered, so the region can't alias them), and
    # compile each as its OWN isolated <region>_<key>.dll that ENTERS at the
    # interior PC (recovers no host). Isolated => a bad fragment fails alone and
    # never poisons a region's trusted DLL.
    def _do_frags(interior_frag_jobs, stats):
        frag_env = dict(os.environ)
        if args.cps:
            frag_env['PSX_CPS'] = '1'
        # Persistent doomed-interior memo: skip interiors already proven to fail
        # audit from IDENTICAL bytes, so autocompile stops re-walking data-as-code
        # every cycle. --force ignores the skip (re-attempts) for triage/debug.
        fail_memo = load_interior_fail_memo(cache_dir)
        memo_skipped = 0
        variant_coverage_cache = {}
        expected_abi = overlay_abi_tag(args.runtime_include, args.flavor)
        runtime_candidate_cap = overlay_candidate_cap(args.runtime_include)
        capacity_probe = os.path.join(
            cache_dir, f'00000000_00000000{overlay_ext()}')
        candidate_capacity_exhausted, existing_candidates = \
            cache_candidate_capacity_full(
                capacity_probe, expected_abi, runtime_candidate_cap)
        if candidate_capacity_exhausted:
            print(f'  overlay candidate capacity already full: '
                  f'{existing_candidates}/{runtime_candidate_cap}; '
                  'new fragments stay interpreted')
        invocation_recipe = (
            expected_abi, int(args.flavor), bool(args.cps), args.compiler,
            hashlib.sha256(json.dumps(
                toml, sort_keys=True, separators=(',', ':'),
                default=str).encode('utf-8')).hexdigest(),
        )
        merged_jobs = merge_fragment_jobs_by_recipe(
            interior_frag_jobs, invocation_recipe)
        if len(merged_jobs) != len(interior_frag_jobs):
            print(f'  fragment recipes: merged {len(interior_frag_jobs)} capture '
                  f'job(s) into {len(merged_jobs)} byte-variant recipe(s)')

        # A namespace that is already saturated cannot admit ordinary fragment
        # growth.  Avoid the expensive cross-variant donor/owner proof at this
        # true fixed point, but preserve the only failure semantics that do not
        # require that proof: a required executed demand already memoized as a
        # deterministic compile failure remains loud.  --force and explicit
        # --force-interior work bypass this shortcut because a same-basename
        # replacement can be capacity-neutral.
        if fragment_capacity_fastpath_eligible(
                candidate_capacity_exhausted, args.force, merged_jobs):
            for job in merged_jobs:
                memo_entries = {}
                for entry in sorted(job['candidates']):
                    key = _interior_fail_key(
                        job['phys_addr'], entry, job['data'],
                        job['load_addr'], job['size'],
                        job['producer_ranges'], job['cross_call_allow'],
                        expected_abi, args.cps, toml)
                    if key in fail_memo:
                        memo_entries[entry] = fail_memo[key]
                if not memo_entries:
                    continue
                current_entries, _current_ranges = \
                    load_region_current_variant_coverage(
                        cache_dir, job['phys_addr'], job['data'],
                        job['load_addr'], job['size'], expected_abi)
                for entry, reason in memo_entries.items():
                    if (entry & 0x1FFFFFFF) in current_entries:
                        continue
                    memo_action = interior_fail_memo_action(
                        entry, job, args.force)
                    if memo_action == 'skip':
                        memo_skipped += 1
                        stats.add_skip()
                    else:
                        stats.add_fail(
                            f'interior 0x{entry:08X} @region '
                            f'0x{job["phys_addr"]:08X}',
                            'fragment_memo', reason)
            stats.add_capacity_fastpath(len(merged_jobs))
            print(f'  fragment capacity fixed point: skipped donor/owner '
                  f'analysis for {len(merged_jobs)} byte-variant recipe job(s); '
                  'the exact unserved hosted-demand count was intentionally '
                  'not recomputed')
            if memo_skipped:
                print(f'  interior fail-memo: skipped {memo_skipped} '
                      'known-doomed optional interior(s)')
            return

        # Global phase 1: publish every strong play-free root before freezing
        # sibling donor authority.  Freezing first deferred these roots until a
        # second CLI invocation: the next run reloaded them from disk and could
        # then nominate hosted interiors that a clean first run missed.  Strong
        # roots are the complete organic authority for cross-variant donors, so
        # closing them for every recipe first makes an empty-cache invocation
        # reproducible without allowing hosted aliases to recurse.
        strong_handled_by_job = []
        for job in merged_jobs:
            phys_addr = job['phys_addr']
            load_addr = job['load_addr']
            size = job['size']
            data = job['data']
            current_variant_ids = load_region_current_variant_func_ids(
                cache_dir, phys_addr, data, load_addr, size, expected_abi,
                include_supplemental=False)
            current_variant_entries, current_variant_ranges = \
                current_variant_func_id_coverage(
                    current_variant_ids, data, load_addr, size)
            strong_handled = set()
            batch_candidates, isolated_candidates = \
                partition_strong_root_demands(
                    job['static_exact_demands'], job['executed'],
                    job['forced'], job['static_interval_demands'],
                    current_variant_entries)
            strong_candidates = batch_candidates + isolated_candidates
            batch_pending = []
            isolated_pending = []
            isolated_candidate_set = set(isolated_candidates)
            for entry in strong_candidates:
                key = _interior_fail_key(
                    phys_addr, entry, data, load_addr, size,
                    job['producer_ranges'], job['cross_call_allow'],
                    expected_abi, args.cps, toml)
                memo_action = interior_fail_memo_action(
                    entry, job, args.force)
                if key in fail_memo and memo_action != 'retry':
                    strong_handled.add(entry)
                    if memo_action == 'skip':
                        memo_skipped += 1
                        stats.add_skip()
                    else:
                        stats.add_fail(
                            f'interior 0x{entry:08X} @region '
                            f'0x{phys_addr:08X}',
                            'fragment_memo', fail_memo[key])
                    continue
                if entry in isolated_candidate_set:
                    isolated_pending.append(entry)
                else:
                    batch_pending.append(entry)

            if candidate_capacity_exhausted:
                capacity_pending = [
                    entry for entry in batch_pending + isolated_pending
                    if not fragment_capacity_allows_entry(
                        True, args.force, job['forced'], entry)
                ]
                if capacity_pending:
                    print(f'  strong-root supplement @0x{phys_addr:08X}: '
                          f'{len(capacity_pending)} demand(s) left to '
                          'interpreter: candidate capacity full')
                    strong_handled.update(capacity_pending)
                    for _entry in capacity_pending:
                        stats.add_skip()
                capacity_pending_set = set(capacity_pending)
                batch_pending = [
                    entry for entry in batch_pending
                    if entry not in capacity_pending_set
                ]
                isolated_pending = [
                    entry for entry in isolated_pending
                    if entry not in capacity_pending_set
                ]
                if not batch_pending and not isolated_pending:
                    strong_handled_by_job.append(strong_handled)
                    continue

            def compile_strong_batch(entries):
                return compile_fragment_batch(
                    entries, data, load_addr, size, phys_addr, cache_dir,
                    args, frag_env, toml, job['producer_ranges'],
                    job['cross_call_allow'])

            def strong_batch_success(entries, frag_ids, status):
                strong_handled.update(entries)
                if status == 'cached':
                    stats.add_skip()
                else:
                    stats.add_ok()
                for ev, _cc, ranges in frag_ids:
                    current_variant_entries.add(ev & 0x1FFFFFFF)
                    current_variant_ranges.extend(
                        ((start & 0x1FFFFFFF),
                         (start & 0x1FFFFFFF) + length)
                        for start, length in ranges)
                current_variant_ids.extend(frag_ids)
                if job.get('resident_cap'):
                    fragment_dll = os.path.join(
                        cache_dir,
                        f'{phys_addr:08X}_{fragment_shard_key(frag_ids):08X}'
                        f'{overlay_ext()}')
                    reconcile_bios_resident_marker(
                        fragment_dll, job['resident_cap'], status == 'built')

            def strong_singleton_failure(entry, status):
                nonlocal candidate_capacity_exhausted
                strong_handled.add(entry)
                if status.startswith('candidate-capacity:'):
                    if status.startswith('candidate-capacity: full'):
                        candidate_capacity_exhausted = True
                    print(f'  fragment 0x{entry:08X} skipped safely: '
                          f'{status}')
                    stats.add_skip()
                    return
                key = _interior_fail_key(
                    phys_addr, entry, data, load_addr, size,
                    job['producer_ranges'], job['cross_call_allow'],
                    expected_abi, args.cps, toml)
                optional_reject = optional_static_fragment_rejection(
                    entry, job, status)
                if optional_reject:
                    print(f'  static fragment 0x{entry:08X} rejected safely: '
                          f'{status}')
                    stats.add_skip()
                else:
                    stats.add_fail(
                        f'interior 0x{entry:08X} @region '
                        f'0x{phys_addr:08X}', 'fragment', status)
                if interior_failure_is_deterministic(status):
                    fail_memo[key] = status
                    append_interior_fail_memo(cache_dir, key, status)

            def strong_batch_failure(entries, status):
                nonlocal candidate_capacity_exhausted
                if status.startswith('candidate-capacity:'):
                    strong_handled.update(entries)
                    if status.startswith('candidate-capacity: full'):
                        candidate_capacity_exhausted = True
                    for _entry in entries:
                        stats.add_skip()
                    print(f'  strong-root batch: {len(entries)} demand(s) '
                          f'left to interpreter: {status}')
                    return
                strong_handled.update(entries)
                stats.add_fail(
                    f'strong-root batch ({len(entries)} entries) '
                    f'@region 0x{phys_addr:08X}',
                    'fragment_batch', status)

            strong_shards = compile_batched_fragment_roots(
                batch_pending, compile_strong_batch, strong_batch_success,
                strong_singleton_failure,
                lambda entry: ((entry & 0x1FFFFFFF) not in
                               current_variant_entries),
                fragment_batch_failure_is_partitionable,
                strong_batch_failure)
            # A root that is also live/forced/interval evidence retains its
            # historical singleton failure domain, but still closes before the
            # donor snapshot. A successful batch may incidentally cover it.
            for entry in isolated_pending:
                if not fragment_capacity_allows_entry(
                        candidate_capacity_exhausted, args.force,
                        job['forced'], entry):
                    if entry not in strong_handled:
                        strong_handled.add(entry)
                        stats.add_skip()
                    continue
                if (entry & 0x1FFFFFFF) in current_variant_entries:
                    strong_handled.add(entry)
                    continue
                frag_ids, status = compile_strong_batch([entry])
                if frag_ids:
                    strong_batch_success([entry], frag_ids, status)
                    strong_shards += 1
                else:
                    strong_singleton_failure(entry, status)
            if strong_candidates:
                print(f'  strong-root supplement @0x{phys_addr:08X}: '
                      f'{len(strong_candidates)} demand(s) -> '
                      f'{strong_shards} batched shard(s)')
            strong_handled_by_job.append(strong_handled)

        # A strong-root publication can consume the last Candidate slots.
        # Re-probe under the shared namespace lock before hosted admission.
        # The outer invocation lock excludes cooperating writers for the rest
        # of this pass; an uncoordinated external deletion can only cause safe
        # coverage loss until the next invocation.
        was_capacity_exhausted = candidate_capacity_exhausted
        candidate_capacity_exhausted, existing_candidates = \
            cache_candidate_capacity_full(
                capacity_probe, expected_abi, runtime_candidate_cap)
        if candidate_capacity_exhausted and not was_capacity_exhausted:
            print(f'  overlay candidate capacity reached after strong-root '
                  f'closure: {existing_candidates}/{runtime_candidate_cap}; '
                  'new fragments stay interpreted')

        # Global phase 2: snapshot native coverage and authority separately.
        # Coverage includes prior hosted shards so a valid alias is not rebuilt.
        # Donor/owner authority excludes provenance-tagged hosted output, whose
        # incidental rooted callees must not manufacture another wave on a later
        # invocation. Strong roots above reconstruct durable authority first.
        initial_variant_ids = [
            load_region_current_variant_func_ids(
                cache_dir, job['phys_addr'], job['data'], job['load_addr'],
                job['size'], expected_abi)
            for job in merged_jobs
        ]
        initial_authority_ids = [
            load_region_current_variant_func_ids(
                cache_dir, job['phys_addr'], job['data'], job['load_addr'],
                job['size'], expected_abi, include_supplemental=False)
            for job in merged_jobs
        ]
        # Build sorted sibling static-donor streams once.  Each recipe needs at
        # most its maximum current-entry population plus one nomination batch:
        # any later address necessarily has HOSTED_NOMINATION_CAP earlier
        # entries from that same sibling after a recipient filters its current
        # coverage.  The recipient heap-merge below applies the final cap only
        # after excluding self evidence and already native entries.
        hosted_donors_by_region = {}
        initial_entry_bound_by_region = Counter()
        for index, indexed_job in enumerate(merged_jobs):
            initial_entry_bound_by_region[indexed_job['phys_addr']] = max(
                initial_entry_bound_by_region[indexed_job['phys_addr']],
                len({_canonical_guest_addr(entry)
                     for entry, _crc, _ranges in
                     initial_authority_ids[index]}))
        hosted_donor_index_drops = Counter()
        for sibling_index, sibling in enumerate(merged_jobs):
            organic_entries = organic_root_entries(
                initial_authority_ids[sibling_index])
            donor_entries = sorted({
                _canonical_guest_addr(entry)
                for entry in sibling.get('hosted_donor_demands', set())
                if _canonical_guest_addr(entry) in organic_entries
            })
            # A recipient can filter no more than its current exact entry set.
            # Normal publication bounds that by the runtime cap; include the
            # observed on-disk maximum as well so a legacy/manual over-cap cache
            # loses coverage safely without invalidating this prefix proof.
            donor_index_limit = (
                max(runtime_candidate_cap,
                    initial_entry_bound_by_region[sibling['phys_addr']]) +
                HOSTED_NOMINATION_CAP)
            dropped = max(0, len(donor_entries) - donor_index_limit)
            if dropped:
                hosted_donor_index_drops[sibling['phys_addr']] += dropped
            hosted_donors_by_region.setdefault(
                sibling['phys_addr'], {})[sibling_index] = tuple(
                    donor_entries[:donor_index_limit])
        for job_index, job in enumerate(merged_jobs):
            phys_addr = job['phys_addr']
            load_addr = job['load_addr']
            size = job['size']
            data = job['data']
            interior_pcs = set(job['candidates'])
            executed = job['executed']
            static_exact_demands = job['static_exact_demands']
            static_interval_demands = job['static_interval_demands']
            forced = job['forced']
            region_crc = binascii.crc32(data) & 0xFFFFFFFF
            # ENTRY-based orphan test, not range-based: native code is
            # enterable only at manifest F entries, so "inside a compiled
            # range" does NOT make a dispatch target servable — a range-
            # covered PC with no F entry anywhere still interps its whole
            # chain on every dispatch. Demand an entry at exactly this PC.
            coverage_key = (phys_addr, hashlib.sha256(data).digest(),
                            load_addr, size)
            current_coverage = variant_coverage_cache.get(coverage_key)
            current_variant_ids = list(initial_variant_ids[job_index])
            current_authority_ids = list(
                initial_authority_ids[job_index])
            current_authority_entries, _authority_ranges = \
                current_variant_func_id_coverage(
                    current_authority_ids, data, load_addr, size)
            if current_coverage is None:
                current_coverage = current_variant_func_id_coverage(
                    current_variant_ids, data, load_addr, size)
                variant_coverage_cache[coverage_key] = current_coverage
            current_variant_entries, current_variant_ranges = current_coverage

            def record_fragment_success(frag_ids, status,
                                        provenance=None):
                if status == 'cached':
                    stats.add_skip()
                else:
                    stats.add_ok()
                for ev, _cc, ranges in frag_ids:
                    current_variant_entries.add(ev & 0x1FFFFFFF)
                    current_variant_ranges.extend(
                        ((start & 0x1FFFFFFF),
                         (start & 0x1FFFFFFF) + length)
                        for start, length in ranges)
                current_variant_ids.extend(frag_ids)
                if job.get('resident_cap'):
                    fragment_dll = os.path.join(
                        cache_dir,
                        f'{phys_addr:08X}_'
                        f'{fragment_shard_key(frag_ids, provenance):08X}'
                        f'{overlay_ext()}')
                    reconcile_bios_resident_marker(
                        fragment_dll, job['resident_cap'], status == 'built')

            def record_fragment_failure(entry, status):
                nonlocal candidate_capacity_exhausted
                if status.startswith('candidate-capacity:'):
                    if status.startswith('candidate-capacity: full'):
                        candidate_capacity_exhausted = True
                    print(f'  fragment 0x{entry:08X} skipped safely: {status}')
                    stats.add_skip()
                    return
                key = _interior_fail_key(
                    phys_addr, entry, data, load_addr, size,
                    job['producer_ranges'], job['cross_call_allow'],
                    expected_abi, args.cps, toml)
                optional_reject = optional_static_fragment_rejection(
                    entry, job, status)
                if optional_reject:
                    print(f'  static fragment 0x{entry:08X} rejected safely: '
                          f'{status}')
                    stats.add_skip()
                else:
                    stats.add_fail(
                        f'interior 0x{entry:08X} @region 0x{phys_addr:08X}',
                        'fragment', status)
                if interior_failure_is_deterministic(status):
                    fail_memo[key] = status
                    append_interior_fail_memo(cache_dir, key, status)

            strong_handled = set(strong_handled_by_job[job_index])

            # Global phase 3: a static root proven in one captured byte recipe
            # can identify an
            # indirect-switch entry in a sibling recipe.  Run this after local
            # strong closure, so owner availability is independent of the
            # poorer base-region bundle.  Donor and owner authority remain
            # frozen to the post-strong snapshot and cannot recurse through a
            # hosted shard. Sibling bytes only nominate the address; all
            # executable authority comes from this recipient's current-byte
            # owner and publication audit.
            sibling_proofs, sibling_proofs_truncated = \
                recipient_sibling_proofs(
                    hosted_donors_by_region.get(phys_addr, {}), job_index,
                    current_authority_entries)
            hosted_audit = {}
            hosted = select_hosted_interior_demands(
                job, sibling_proofs, current_authority_ids, hosted_audit)
            if sibling_proofs_truncated:
                hosted_audit['region_nomination_cap_truncated'] = 1
            donor_index_drops = hosted_donor_index_drops.get(phys_addr, 0)
            if donor_index_drops:
                hosted_audit['region_donor_index_cap_dropped'] = \
                    donor_index_drops

            def compile_hosted_batch(specs):
                return compile_fragment_batch(
                    specs, data, load_addr, size, phys_addr, cache_dir,
                    args, frag_env, toml, job['producer_ranges'],
                    job['cross_call_allow'], hosted_owners=specs)

            hosted_rejected = set()
            hosted_published = False

            def hosted_success(batch, frag_ids, status):
                nonlocal hosted_published
                record_fragment_success(
                    frag_ids, status, HOSTED_MANIFEST_PROVENANCE)
                if status == 'built':
                    hosted_published = True
                emitted = {
                    _canonical_guest_addr(entry)
                    for entry, _crc, _ranges in frag_ids
                }
                missing = sorted(set(batch) - emitted)
                if missing:
                    print(f'  hosted batch: {len(batch) - len(missing)}/'
                          f'{len(batch)} target(s) passed recipient CFG proof; '
                          f'{len(missing)} retrying in smaller batches')
                return set(batch) - set(missing)

            def hosted_singleton_failure(entry, status):
                nonlocal candidate_capacity_exhausted
                print(f'  hosted interior 0x{entry:08X} rejected safely: '
                      f'{status}')
                if status.startswith('candidate-capacity: full'):
                    candidate_capacity_exhausted = True
                    if args.force:
                        stats.add_skip()
                    return
                hosted_rejected.add(_canonical_guest_addr(entry))
                if status.startswith((
                        'generated-c-audit:', 'hosted-entry-audit:',
                        'hosted-no-target:', 'candidate-capacity:',
                        'no-generated-output', 'no-func-ids')):
                    stats.add_skip()
                else:
                    stats.add_fail(
                        f'hosted interior 0x{entry:08X} '
                        f'@region 0x{phys_addr:08X}',
                        'hosted_fragment', status)

            def hosted_batch_failure(batch, status):
                nonlocal candidate_capacity_exhausted
                if status.startswith('candidate-capacity: full'):
                    candidate_capacity_exhausted = True
                    print(f'  hosted batch: 0/{len(batch)} target(s) passed '
                          'recipient CFG proof; candidate capacity full')
                    if args.force:
                        for _entry in batch:
                            stats.add_skip()
                    return
                hosted_rejected.update(
                    _canonical_guest_addr(entry) for entry in batch)
                if status.startswith((
                        'hosted-no-target:',
                        'hosted-entry-audit: compile attempt cap',
                        'candidate-capacity:', 'no-generated-output',
                        'no-func-ids')):
                    for _entry in batch:
                        stats.add_skip()
                    print(f'  hosted batch: 0/{len(batch)} target(s) passed '
                          'recipient CFG proof; rejected safely')
                    return
                stats.add_fail(
                    f'hosted-interior batch ({len(batch)} entries) '
                    f'@region 0x{phys_addr:08X}',
                    'hosted_fragment_batch', status)

            hosted_shards = compile_batched_hosted_aliases(
                hosted, compile_hosted_batch, hosted_success,
                hosted_singleton_failure,
                lambda entry: ((entry & 0x1FFFFFFF) not in
                               current_variant_entries),
                fragment_batch_failure_is_partitionable,
                hosted_batch_failure,
                stop_requested=lambda: (
                    candidate_capacity_exhausted and not args.force))
            if hosted_published:
                was_capacity_exhausted = candidate_capacity_exhausted
                candidate_capacity_exhausted, existing_candidates = \
                    cache_candidate_capacity_full(
                        capacity_probe, expected_abi, runtime_candidate_cap)
                if (candidate_capacity_exhausted and
                        not was_capacity_exhausted):
                    print(f'  overlay candidate capacity reached after '
                          f'hosted closure: {existing_candidates}/'
                          f'{runtime_candidate_cap}; ordinary fragments stay '
                          'interpreted')
            hosted_capacity_skipped = set()
            if candidate_capacity_exhausted and not args.force:
                already_accounted = hosted_rejected | {
                    _canonical_guest_addr(entry) for entry in strong_handled
                }
                capacity_missing = hosted_entries_still_missing(
                    hosted, current_variant_entries, already_accounted)
                if capacity_missing:
                    hosted_capacity_skipped.update(capacity_missing)
                    hosted_audit['candidate_capacity_full'] = \
                        len(capacity_missing)
                    for _entry in capacity_missing:
                        stats.add_skip()
            if sibling_proofs:
                print(f'  cross-variant hosted supplement @0x{phys_addr:08X}: '
                      f'{len(hosted)}/{len(sibling_proofs)} sibling-proven '
                      f'target(s) had a unique current-byte local owner -> '
                      f'{hosted_shards} shard(s); '
                      f'audit={dict(sorted(hosted_audit.items()))}')

            orphans = select_fragment_orphans(
                interior_pcs, executed, forced, static_exact_demands,
                static_interval_demands, current_variant_entries,
                current_variant_ranges)
            orphans = [entry for entry in orphans
                       if (entry not in strong_handled and
                           _canonical_guest_addr(entry) not in
                           hosted_capacity_skipped)]
            if not orphans:
                continue
            capacity_orphans = [
                entry for entry in orphans
                if not fragment_capacity_allows_entry(
                    candidate_capacity_exhausted, args.force,
                    job['forced'], entry)
            ]
            if capacity_orphans:
                print(f'  orphan fragments: {len(capacity_orphans)} '
                      'demand(s) left to interpreter: candidate capacity full')
                for _entry in capacity_orphans:
                    stats.add_skip()
                capacity_orphan_set = set(capacity_orphans)
                orphans = [
                    entry for entry in orphans
                    if entry not in capacity_orphan_set
                ]
                if not orphans:
                    continue
            built = 0
            for a in orphans:
                # A preceding orphan can consume the final slot or receive the
                # first authoritative full verdict.  Stop ordinary toolchain
                # work immediately while still allowing a later explicit
                # capacity-neutral repair candidate.
                if not fragment_capacity_allows_entry(
                        candidate_capacity_exhausted, args.force,
                        job['forced'], a):
                    stats.add_skip()
                    continue
                key = _interior_fail_key(
                    phys_addr, a, data, load_addr, size,
                    job['producer_ranges'], job['cross_call_allow'],
                    expected_abi, args.cps, toml)
                memo_action = interior_fail_memo_action(a, job, args.force)
                if key in fail_memo and memo_action != 'retry':
                    # Deterministically doomed from these exact bytes — skip the
                    # ~16k-word data walk. NOT a fresh failure (already counted
                    # when first memoized); count as a skip so the summary is honest.
                    if memo_action == 'skip':
                        memo_skipped += 1
                        stats.add_skip()
                    else:
                        stats.add_fail(
                            f'interior 0x{a:08X} @region 0x{phys_addr:08X}',
                            'fragment_memo', fail_memo[key])
                    continue
                frag_ids, status = compile_interior_fragment(
                    a, data, load_addr, size, phys_addr, cache_dir, args,
                    frag_env, toml, job['producer_ranges'],
                    job['cross_call_allow'])
                if frag_ids:
                    built += 1
                    record_fragment_success(
                        frag_ids, status, ORPHAN_MANIFEST_PROVENANCE)
                else:
                    # An executed orphan interior that would NOT build is a real
                    # coverage hole: that PC's whole dispatch chain runs
                    # interpreted forever. Tally it loudly with the reason.
                    # Persist only deterministic generated-code audit verdicts;
                    # compiler/spawn errors and cache-pair mismatches depend on
                    # mutable external state and must remain retryable.
                    record_fragment_failure(a, status)
            print(f'  interior fragments @0x{phys_addr:08X}: {built}/{len(orphans)} '
                  f'exact-demand orphan interior(s) -> isolated island shards')
        if memo_skipped:
            print(f'  interior fail-memo: skipped {memo_skipped} known-doomed '
                  f'interior(s) (data-as-code from identical bytes; '
                  f'{INTERIOR_FAIL_MEMO})')

    # ---- Drive the capture list -------------------------------------------
    # Dynamic cache publication is deliberately canonical and sequential. A
    # parallel recompile race is harmless below capacity but chooses an
    # irreproducible winning subset at the hard Candidate boundary. Static
    # combined-C mode has no runtime cache publication and keeps its historical
    # capture order.
    stats = ShardStats()
    if args.static:
        for cap in captures:
            _do_capture(cap, region_coverage_cache, interior_frag_jobs, stats)
    else:
        # Publication order is part of the reproducible cache result whenever
        # the process-lifetime Candidate cap can bind. Sort complete capture
        # recipes and serialize whole compiler invocations in this namespace;
        # a mutex around only the final rename would still admit whichever
        # worker happened to arrive first at cap-minus-N.
        capacity_lock, _tier_dirs = _candidate_capacity_namespace(
            os.path.join(cache_dir, f'00000000_00000000{overlay_ext()}'))
        invocation_lock = capacity_lock + '.invocation'
        with _exclusive_file_lock(invocation_lock, timeout=None):
            expected_abi = overlay_abi_tag(
                args.runtime_include, args.flavor)
            runtime_candidate_cap = overlay_candidate_cap(
                args.runtime_include)
            capacity_probe = os.path.join(
                cache_dir, f'00000000_00000000{overlay_ext()}')
            existing_candidates, candidate_counts = \
                cache_candidate_capacity_snapshot(
                    capacity_probe, expected_abi)
            saturated_single_tier = candidate_capacity_saturated_in_tier(
                existing_candidates, candidate_counts,
                runtime_candidate_cap, cache_dir)
            capacity_fixedpoint = (
                saturated_single_tier and not args.force and
                not forced_interiors)
            captures_to_process = captures
            if capacity_fixedpoint:
                # At a single-tier full namespace, ordinary compilation cannot
                # publish growth and there is no lower compiler tier to promote
                # with a capacity-neutral basename replacement.  This is an
                # intentionally coarse fixed-point result: coverage_report is
                # the exact gap authority, while this invocation avoids decoding
                # and sorting a potentially gigabyte-scale capture collection.
                # BIOS resident bundles are the exception: normal policy lets a
                # richer capture replace its canonical bundle without --force,
                # and that replacement can be capacity-neutral. Process only
                # these tiny captures, then re-probe before suppressing the
                # gigabyte-scale ordinary collection.
                resident_captures = [
                    cap for cap in captures
                    if cap.get('producer') == BIOS_RESIDENT_PRODUCER
                ]
                resident_captures.sort(key=lambda cap: (
                    int(cap['load_addr'], 16) & 0x1FFFFFFF,
                    binascii.crc32(base64.b64decode(
                        cap['bytes_b64'])) & 0xFFFFFFFF,
                    hashlib.sha256(json.dumps(
                        cap, sort_keys=True, separators=(',', ':'),
                        default=str).encode('utf-8')).digest(),
                ))
                for cap in resident_captures:
                    _do_capture(
                        cap, region_coverage_cache, interior_frag_jobs, stats)
                resident_ids = {id(cap) for cap in resident_captures}
                captures_to_process = [
                    cap for cap in captures if id(cap) not in resident_ids
                ]
                existing_candidates, candidate_counts = \
                    cache_candidate_capacity_snapshot(
                        capacity_probe, expected_abi)
                capacity_fixedpoint = candidate_capacity_saturated_in_tier(
                    existing_candidates, candidate_counts,
                    runtime_candidate_cap, cache_dir)
                if capacity_fixedpoint:
                    stats.add_capacity_fastpath(len(captures_to_process))
                    print(f'Overlay candidate fixed point: '
                          f'{existing_candidates}/{runtime_candidate_cap} '
                          f'candidates in the active compiler tier; processed '
                          f'{len(resident_captures)} BIOS resident recipe(s) '
                          f'and skipped {len(captures_to_process)} ordinary '
                          'capture recipe(s) before byte decode. Use --force or '
                          '--force-interior for capacity-neutral repair '
                          'attempts; coverage_report remains the exact gap '
                          'authority.\n')
            if not capacity_fixedpoint:
                captures_to_process.sort(key=lambda cap: (
                    int(cap['load_addr'], 16) & 0x1FFFFFFF,
                    binascii.crc32(base64.b64decode(
                        cap['bytes_b64'])) & 0xFFFFFFFF,
                    hashlib.sha256(json.dumps(
                        cap, sort_keys=True, separators=(',', ':'),
                        default=str).encode('utf-8')).digest(),
                ))
                if args.jobs > 1:
                    print('Deterministic candidate-cap admission: region '
                          'recipes run in canonical order '
                          '(parallel publication disabled)\n')
                for cap in captures_to_process:
                    _do_capture(
                        cap, region_coverage_cache, interior_frag_jobs, stats)
                _do_frags(interior_frag_jobs, stats)

    # B-2: write combined static C file
    if args.static and static_parts:
        # CPS can yield at every compiled block leader, not just leaders that a
        # particular capture happened to observe. Give every block in every
        # variant a content-gated resume wrapper. This makes static coverage
        # independent of host timing / slice boundaries.
        synthesized = 0

        def synthesize_all_resume_wrappers(parts):
            nonlocal synthesized
            for part in parts:
                done = part.setdefault('resume_entries', set())
                for entry, host in sorted(part['continuation_owners'].items()):
                    if entry in part['func_addrs'] or entry in done:
                        continue
                    if host not in part['ids_by_addr']:
                        continue
                    symbol = f'{part["namespace"]}_func_{entry:08X}'
                    host_symbol = part['symbols'][host]
                    part['src'], resume_ok = add_cps_resume_case(
                        part['src'], host_symbol, host, entry)
                    if not resume_ok:
                        continue
                    part['src'] += (
                        f'\n/* CPS block resume entry owned by 0x{host:08X}. */\n'
                        f'void {symbol}(CPUState* cpu)\n{{\n'
                        f'    cpu->pc = 0x{entry:08X}u;\n'
                        f'    {host_symbol}(cpu);\n'
                        f'}}\n')
                    for code_crc, ranges in part['ids_by_addr'][host]:
                        part['variants'].append({
                            'addr': entry,
                            'symbol': symbol,
                            'crc': code_crc,
                            'ranges': ranges,
                        })
                    done.add(entry)
                    synthesized += 1

        synthesize_all_resume_wrappers(static_parts)
        existing_entries = {
            variant['addr']
            for part in static_parts
            for variant in part['variants']
        }

        # Captured entries not owned by any compiled host are genuine orphan
        # interiors. Compile each as an isolated dispatch-root shard, then give
        # every block in those fragments the same universal resume treatment.
        unresolved = sorted(static_requested_entries - existing_entries)
        fragment_built = 0
        new_fragment_parts = []
        for entry in unresolved:
            if entry in existing_entries:
                continue
            source = static_entry_sources.get(entry)
            if source is None:
                continue
            data, load_addr, size, phys_addr = source
            part = generate_interior_fragment_static(
                entry, data, load_addr, size, phys_addr, args)
            if part is None:
                continue
            static_parts.append(part)
            new_fragment_parts.append(part)
            existing_entries.update(
                variant['addr'] for variant in part['variants'])
            fragment_built += 1

        synthesize_all_resume_wrappers(new_fragment_parts)
        existing_entries = {
            variant['addr']
            for part in static_parts
            for variant in part['variants']
        }
        unresolved = sorted(static_requested_entries - existing_entries)
        print(f'Static universal CPS resume wrappers: {synthesized}')
        print(f'Static isolated interior shards: {fragment_built}')
        if unresolved:
            sample = ', '.join(f'0x{entry:08X}' for entry in unresolved[:12])
            print(f'STATIC COVERAGE WARNING: {len(unresolved)} captured dispatch '
                  f'entry(s) have no compiled body/owner: {sample}')
            for entry in unresolved:
                stats.add_fail(f'static entry 0x{entry:08X}', 'static_unresolved',
                               'captured dispatch entry with no compiled body/owner')

        all_variants = []
        combined = '/* Auto-generated overlay dispatch — do not edit.\n'
        combined += ' * Rebuild: python3 psxrecomp/tools/compile_overlays.py --static ...\n'
        combined += ' */\n'
        for part in static_parts:
            combined += part['src']
            all_variants.extend(part['variants'])
        combined += generate_overlay_dispatch(all_variants)
        with open(static_out, 'w') as f:
            f.write(combined)
        print(f'Static output: {static_out}  '
              f'({len(all_variants)} exact function identities total)')

    # LOUD summary + machine-readable result line, then a non-zero exit when any
    # shard that should have built failed. The runtime's autocompile watcher and
    # any CI/dev invocation now SEE shard failures instead of a green exit 0 that
    # masked a header-drift breakage. Skips (data-only / already-covered) and a
    # zero-capture run are NOT failures.
    n_fail = stats.print_summary()
    if _check_tmp:
        import shutil
        shutil.rmtree(_check_tmp, ignore_errors=True)
        print(f'[check] removed throwaway dir {_check_tmp}')
    print('Done.')
    if n_fail:
        raise SystemExit(2)


if __name__ == '__main__':
    main()
