#!/usr/bin/env python3
"""release_stage.py — THE release staging implementation, for every psxrecomp
title, on every platform.

WHY THIS FILE EXISTS
====================
Release packaging is the same job on Windows and Linux: derive the overlay
codegen cache tag, copy the shards that carry it, copy the overlay toolchain,
copy the mod catalog, and refuse to produce a package when any of that is
missing. Until now it was implemented FIVE times:

  * tools/release_overlay_stage.ps1 in this repo (the shared Windows module,
    added by beads-eio.2.6 after a packager that re-derived the tag in
    PowerShell staged ZERO shards from a perfectly good cache), and
  * tools/package_appimage.sh forked into ApeEscapeRecomp (416 lines),
    Tomba2Recomp (436) and MegaManX6Recomp (408), each with its own copy of
    the tag derivation. (Line counts are of each repo's origin/master. Reading
    a working tree gives the wrong answer here -- the Ape checkout is parked on
    an old branch and reports 403.)

Every one of those four hand-built the tag string. Measured 2026-09-02, on each
repo's origin/master:

    repo             composes _f   calls cache_tag()   stages toolchain
    ApeEscapeRecomp       yes            no                  no
    Tomba2Recomp          NO             no                  no
    MegaManX6Recomp       NO             no                  no

The `_f<flavor>` field was added to the tag in the framework. Exactly one of the
three forks was hand-patched to append it (ApeEscapeRecomp 4a17272, 2026-08-28).
The other two still emit `cg<N>_<hash>_gc<hash>`, and their shard filter is
`find -path "*/$cg_tag/*"`, which requires a slash immediately after the tag.
The real directory is `..._f0/`, so the pattern cannot match, `shards` evaluates
to 0, and the packager exits 1. That is why Tomba 2 v0.0.9 shipped Windows-only:
there is no AppImage asset in that release at all. See bead beads-eio.3.102.

A fix that has to be applied N times gets applied fewer than N times. So the
logic lives here, once, and the platform scripts are argument marshalling:

    tools/release_overlay_stage.ps1   dot-sourced by a title's package_release.ps1
    tools/release_overlay_stage.sh    sourced by a title's package_appimage.sh

Both are THIN. Neither contains a tag format string, a shard filter, or a
catalog rule; if you are about to add one to either, add it here instead.

THE TAG COMES FROM compile_overlays.cache_tag(), FULL STOP
==========================================================
`cache_tag()` (tools/compile_overlays.py) is the single source of truth and must
stay identical to overlay_loader.c's scan_cache_dir(). This file imports it and
calls it. It does not know the tag's shape and must never learn it — the whole
defect class above is what a second copy of that format string costs. The same
applies to the arch-abi segment and the shared-library suffix: `cache_arch_abi()`
and `overlay_ext()` come from the same module rather than being re-derived.

Usage (see each subcommand's --help):
    python3 tools/release_stage.py cg-tag          ...
    python3 tools/release_stage.py stage-cache     ...
    python3 tools/release_stage.py stage-toolchain ...
    python3 tools/release_stage.py stage-mods      ...
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import tarfile
import time
import urllib.request
import zipfile

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

# The module that OWNS the cache layout. Everything tag-shaped or path-shaped
# below is asked of it.
import compile_overlays as co  # noqa: E402


class StageError(Exception):
    """A staging failure that must stop the release, not scroll past in a log."""


def _die(msg):
    raise StageError(msg)


def _mkdirs(path):
    os.makedirs(path, exist_ok=True)
    return path


def _url_basename(url):
    """Download-cache filename for `url`.

    Percent-escapes are decoded so the cached name matches the publisher's
    asset name: python-build-standalone's URLs carry `%2B` where the version
    has a `+`, and a cache entry named `...3.13.1%2B20250115...` would be
    fetched again by any future reader that decoded it.
    """
    from urllib.parse import unquote, urlsplit
    return os.path.basename(unquote(urlsplit(url).path))


# ---------------------------------------------------------------------------
# Artifact classification
# ---------------------------------------------------------------------------
# The shippable triple. `.resident` is included on purpose: both platforms
# already agreed on it (the PowerShell -Include list and the shell find's
# -name set both carried it) and they must keep agreeing.
def _shippable_suffixes(shared_ext):
    return (shared_ext, '.ranges', '.resident')


# Things that exist in a producer's cache directory and MUST NOT ship.
#
# Selection is the suffix ALLOWLIST above, so a new artifact kind appearing in a
# producer's cache is excluded by default rather than shipped by default. This
# denylist is checked FIRST anyway, for two reasons: some producer-internal
# files end in a shippable suffix (see the temp-shard note below), and the
# staged tree is re-scanned against this same list after copying, so a bug in
# the selection code fails the release instead of shipping a memo that
# suppresses the runtime's ABI preflight.
#
#   .abi_<tag>.ok   a completed-sweep memo. The runtime's ABI preflight skips
#                   re-verifying every shard when it finds this file. Shipping
#                   it tells a player's first launch that a sweep it never ran
#                   already passed, which is precisely how a stale shard gets
#                   loaded instead of rejected.
#   *.c             the generated C the shard was compiled from. Large, and it
#                   is producer input, not a runtime artifact.
#   *.pair-lock     a producer-side concurrency lock. Meaningless to a player
#                   and actively confusing if a stale one ships.
#   .<name>.so.tmp.<rand>.so   an IN-PROGRESS shard. compile_overlays writes
#                   each shard to a hidden temp name and renames it into place,
#                   so a cache directory that is being written, or was written
#                   by an interrupted run, holds files that END IN THE SHIPPABLE
#                   SUFFIX. Measured 2026-09-02 in a live Tomba 2 cache build:
#                     .00096000_407B1780.so.tmp.ryt4jqca.so
#                     .00096000_407B1780.so.tmp.ryt4jqca.ranges
#                   A suffix allowlist alone therefore SHIPS them, and so did
#                   the old shell `find -name '*.so'`. Every producer-internal
#                   file observed in a real cache is HIDDEN, and no shippable
#                   artifact ever is, so a leading dot is the reliable
#                   discriminator and is applied before the suffix test.
_FORBIDDEN_PATTERNS = (
    (re.compile(r'^\.abi_.*\.ok$'), 'ABI sweep memo (suppresses the runtime ABI preflight)'),
    (re.compile(r'\.c$'), 'generated C source (producer input, not a runtime artifact)'),
    (re.compile(r'\.pair-lock$'), 'producer-side concurrency lock'),
    (re.compile(r'^\.'), 'producer-internal hidden file (in-progress shard, lock '
                         'or sweep memo); no shippable cache artifact is hidden'),
    (re.compile(r'\.tmp\.'), 'in-progress producer temp file'),
)


def _forbidden_reason(name):
    for pat, why in _FORBIDDEN_PATTERNS:
        if pat.search(name):
            return why
    return None


# ---------------------------------------------------------------------------
# cg-tag
# ---------------------------------------------------------------------------
# PSX_OVERLAY_FLAVOR is a CODEGEN flavor, not a platform: overlay_api.h defines
# it as 0 (base) unless the runtime target is compiled with a flavor define,
# and runtime/runtime.cmake sets PSX_OVERLAY_FLAVOR=2 (PSX_OVERLAY_FLAVOR_PGXP)
# for a PGXP target. Windows and Linux builds of the SAME target therefore have
# the SAME flavor. There is deliberately no default here: a packager that
# guesses 0 for a PGXP runtime writes a tag the shipped binary never reads, and
# ships a cache that silently does nothing. Pass --flavor explicitly, or better,
# --flavor-from-build so the value comes from the build that produced the binary
# being packaged.
_FLAVOR_FILE = 'psxrecomp_overlay_flavor-%s.txt'


def _flavor_from_build(build_dir, target):
    path = os.path.join(build_dir, _FLAVOR_FILE % target)
    if not os.path.isfile(path):
        _die("cannot read the overlay codegen flavor for target '%s': %s does "
             "not exist.\n"
             "  runtime/runtime.cmake publishes that file for every target it "
             "creates, so either the build directory is wrong, the target name "
             "is wrong, or the framework predates the flavor publication "
             "(bead beads-eio.3.102). Build the runtime target first, or pass "
             "--flavor explicitly if you are certain of the value."
             % (target, path))
    with open(path) as f:
        text = f.read().strip()
    if not re.fullmatch(r'\d+', text):
        _die('%s does not contain an integer flavor (read %r)' % (path, text))
    return int(text)


def _require_built_codegen_hash(runtime_include):
    """Refuse to derive a release tag from an UNBUILT runtime include tree.

    compile_overlays.codegen_hash() returns 0 when overlay_codegen_hash.h is
    absent, and that fallback is correct for its purpose: overlay_api.h has the
    same __has_include fallback, so a runtime built without the header and a
    compiler run without the header agree on cg<N>_00000000_... and the cache
    still loads.

    It is never correct for a RELEASE. The header is generated by the runtime
    BUILD (runtime.cmake's "Hashing recompiler codegen" step), so a packager
    that reads a never-built tree silently produces a tag with a zeroed codegen
    hash -- a plausible-looking string that names a namespace the shipped binary
    does not read. Measured 2026-09-02: a Tomba 2 tag derived before the runtime
    build came out as cg10_00000000_gc530dff78_f0 instead of
    cg10_ecd487f7_gc530dff78_f0.

    Failing here rather than warning is the point of this whole bead: a wrong
    tag does not break anything a build step can see. It ships.
    """
    if co.codegen_hash(runtime_include) != 0:
        return
    _die('the overlay codegen hash reads as 0, which means %s/'
         'overlay_codegen_hash.h does not exist.\n'
         '  That header is generated by the runtime BUILD. Deriving a release '
         'tag without it produces cg<N>_00000000_..., which is a namespace the '
         'shipped binary does not read -- the package would carry a cache the '
         'runtime ignores completely, and nothing would fail.\n'
         '  Build the runtime target first (that also publishes the flavor '
         'file), then re-run the packager.' % runtime_include)


def cmd_cg_tag(args):
    inc = os.path.abspath(args.runtime_include)
    _require_built_codegen_hash(inc)
    if args.flavor is None:
        flavor = _flavor_from_build(args.flavor_from_build, args.runtime_target)
    else:
        flavor = args.flavor
    # THE call. Never a format string here.
    tag = co.cache_tag(inc,
                       os.path.abspath(args.recompiler),
                       os.path.abspath(args.game_toml),
                       flavor)
    if not tag:
        _die('compile_overlays.cache_tag() returned nothing')
    print(tag)
    return 0


# ---------------------------------------------------------------------------
# stage-cache
# ---------------------------------------------------------------------------
def _split_rel(rel):
    return [p for p in rel.replace('\\', '/').split('/') if p not in ('', '.')]


def stage_cache(game_id, cache_src_root, stage, cg_tag,
                arch_abi=None, shared_ext=None, allow_empty_reason=None,
                log=print):
    """Copy the overlay shards carrying `cg_tag` into `<stage>/cache/<game_id>`.

    The loader scans exactly `cache/<game_id>/<tier>/<arch-abi>/<tag>/`
    (overlay_loader.c scan_cache_dir / warn_on_cgtag_mismatch). A shard that
    lands anywhere else is worth exactly as much as no shard at all, and a
    count can never tell the difference — so the SHAPE is what is matched, not
    a substring of the path. The old shell packagers used
    `find -path "*/$cg_tag/*"`, a substring match that is simultaneously too
    loose (it would accept a foreign arch-abi) and, because it needs a
    separator right after the tag, too strict (it rejected the real `_f0`
    directory and staged nothing at all).

    Returns (staged_shard_count, tag_dirs_seen).
    """
    arch_abi = arch_abi or co.cache_arch_abi()
    shared_ext = shared_ext or co.overlay_ext()
    keep_suffixes = _shippable_suffixes(shared_ext)

    cache_src = os.path.join(cache_src_root, game_id)
    if not os.path.isdir(cache_src):
        if allow_empty_reason:
            log('release_stage: NO overlay cache at %s; shipping without one '
                'because the caller declared: %s' % (cache_src, allow_empty_reason))
            return 0, []
        _die(_no_cache_message(cache_src, cg_tag, cache_src_root, arch_abi,
                               shared_ext, found_any=False))

    selected = []          # (abs_src, rel_dst)
    wrong_arch = set()
    other_tags = set()
    excluded = []
    for dirpath, dirnames, filenames in os.walk(cache_src):
        rel_dir = os.path.relpath(dirpath, cache_src)
        parts = _split_rel(rel_dir)
        if not filenames:
            continue
        # Expected shape: <tier>/<arch-abi>/<tag>[/...]
        if len(parts) < 3:
            continue
        tier, seen_arch, seen_tag = parts[0], parts[1], parts[2]
        if tier == 'sljit':
            continue           # tier removed from the runtime (PR #46)
        if seen_tag.startswith('cg') and seen_tag != cg_tag:
            other_tags.add('%s/%s/%s' % (tier, seen_arch, seen_tag))
            continue
        if seen_tag != cg_tag:
            continue
        if seen_arch != arch_abi:
            wrong_arch.add('%s/%s/%s' % (tier, seen_arch, seen_tag))
            continue
        for name in filenames:
            # FORBIDDEN IS CHECKED FIRST, and beats the suffix allowlist. It has
            # to: an in-progress shard is named .<crc>.so.tmp.<rand>.so, so it
            # ends in a shippable suffix and a suffix-first test ships it. That
            # is not a hypothetical layout -- it is what a cache directory looks
            # like while it is being written, or after an interrupted run.
            why = _forbidden_reason(name)
            if why:
                excluded.append((os.path.join(rel_dir, name), why))
                continue
            if not name.endswith(keep_suffixes):
                continue
            selected.append((os.path.join(dirpath, name),
                             os.path.join(rel_dir, name)))

    if not selected:
        if allow_empty_reason:
            log('release_stage: overlay cache at %s holds no %s shards for tag '
                '%s; shipping without one because the caller declared: %s'
                % (cache_src, arch_abi, cg_tag, allow_empty_reason))
            return 0, []
        _die(_no_cache_message(cache_src, cg_tag, cache_src_root, arch_abi,
                               shared_ext, found_any=True,
                               other_tags=sorted(other_tags),
                               wrong_arch=sorted(wrong_arch)))

    cache_dst = os.path.join(stage, 'cache', game_id)
    for src, rel in selected:
        dst = os.path.join(cache_dst, rel)
        _mkdirs(os.path.dirname(dst))
        shutil.copy2(src, dst)

    # ---- assert the STAGED layout, not the copied count -------------------
    staged_shards = []
    tag_dirs = set()
    for dirpath, _dirnames, filenames in os.walk(cache_dst):
        for name in filenames:
            full = os.path.join(dirpath, name)
            why = _forbidden_reason(name)
            if why:
                _die('release staging produced a file that must never ship: %s '
                     '(%s). This is a bug in release_stage.py, not in the cache.'
                     % (os.path.relpath(full, stage), why))
            if name.endswith(shared_ext):
                staged_shards.append(full)
                parts = _split_rel(os.path.relpath(dirpath, cache_dst))
                if len(parts) >= 3:
                    tag_dirs.add(parts[2])

    if len(tag_dirs) != 1 or cg_tag not in tag_dirs:
        _die('staged overlay cache should live under exactly one tag directory '
             '(%s) but the staged tree has %r. The loader scans '
             'cache/%s/<tier>/%s/%s/ exactly, so anything else would never load.'
             % (cg_tag, sorted(tag_dirs), game_id, arch_abi, cg_tag))

    n = len(staged_shards)
    log('Bundled overlay cache: %d native overlay %s [%s/%s]'
        % (n, shared_ext, arch_abi, cg_tag))
    if other_tags:
        log('release_stage: note - the source cache also holds shards under '
            'foreign tag(s) %s; those are NOT shippable (the runtime ignores '
            'other namespaces) and were skipped.' % ', '.join(sorted(other_tags)))
    # Report exclusions as a grouped tally rather than a line each: a real
    # cache holds one .pair-lock and one .c PER SHARD, so per-file lines would
    # bury the shard count under hundreds of notes in every release log.
    if excluded:
        tally = {}
        for _rel, why in excluded:
            tally[why] = tally.get(why, 0) + 1
        for why in sorted(tally):
            log('release_stage: excluded %d file(s): %s' % (tally[why], why))
    return n, sorted(tag_dirs)


def _no_cache_message(cache_src, cg_tag, cache_src_root, arch_abi, shared_ext,
                      found_any, other_tags=(), wrong_arch=()):
    detail = []
    if not found_any:
        detail.append('There is no directory at %s at all.' % cache_src)
    else:
        detail.append('The directory %s exists but holds no %s/%s shards.'
                      % (cache_src, arch_abi, cg_tag))
    if other_tags:
        detail.append('It DOES hold shards under: %s. That is the read-tag != '
                      'write-tag failure: the cache was built against a '
                      'different runtime/config than the one being packaged, '
                      'so the shipped binary would ignore every shard.'
                      % ', '.join(other_tags))
    if wrong_arch:
        detail.append('It DOES hold shards for a different arch-abi: %s. This '
                      'package stages %s.' % (', '.join(wrong_arch), arch_abi))
    return (
        'REFUSING TO PACKAGE WITHOUT AN OVERLAY CACHE.\n\n'
        + '\n'.join('  ' + d for d in detail) + '\n\n'
        'Shipping with no cache is a real downgrade, not a cosmetic gap: every\n'
        'player\'s first visit to every area runs its overlays on the dirty-RAM\n'
        'interpreter. Build a cache for THIS release\'s tag (%s). The tag is\n'
        'derived from the PACKAGED game.toml, so a cache built against the dev\n'
        'game.toml lands under a different tag and the shipped runtime silently\n'
        'ignores it:\n\n'
        '  python3 <framework>/tools/compile_overlays.py \\\n'
        '      --captures        <coverage vault>/overlay_captures.json \\\n'
        '      --game-toml       <the STAGED game.toml> \\\n'
        '      --recompiler      <framework>/recompiler/build*/psxrecomp-game \\\n'
        '      --runtime-include <framework>/runtime/include \\\n'
        '      --out-dir         %s \\\n'
        '      --gcc             $(command -v gcc)\n\n'
        'Run it with the interpreter for the TARGET platform: shards are %s\n'
        'under <tier>/%s/, and compile_overlays decides both from the host it\n'
        'runs on.' % (cg_tag, cache_src_root, shared_ext, arch_abi))


def cmd_stage_cache(args):
    reason = args.ship_without_overlay_cache_because
    n, _tags = stage_cache(args.game_id, args.cache_src_root, args.stage,
                           args.cg_tag, arch_abi=args.arch_abi,
                           shared_ext=args.shared_ext,
                           allow_empty_reason=reason)
    if args.count_file:
        with open(args.count_file, 'w') as f:
            f.write('%d\n' % n)
    return 0


# ---------------------------------------------------------------------------
# stage-toolchain
# ---------------------------------------------------------------------------
# Pinned toolchain archives. Single source of truth for every title and every
# platform: a game cannot ship an unpinned toolchain by forgetting to pass a
# hash, and the two platforms cannot pin different Python versions.
TOOLCHAIN_PINS = {
    'win': {
        'python_version': '3.13.1',
        'python_url': 'https://www.python.org/ftp/python/3.13.1/python-3.13.1-embed-amd64.zip',
        'python_sha256': '7b7923ff0183a8b8fca90f6047184b419b108cb437f75fc1c002f9d2f8bcec16',
        'tcc_version': '0.9.27',
        'tcc_url': 'https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip',
        'tcc_sha256': '34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2',
    },
    # Linux has no equivalent of python.org's embeddable zip, so the pinned
    # interpreter is a python-build-standalone release: a relocatable CPython
    # that needs nothing installed on the player's machine. Same 3.13.1 as the
    # Windows pin, deliberately -- one Python version across both platforms.
    # The `_stripped` asset is used (21 MB vs 75 MB download, 78 MB unpacked);
    # compile_overlays.py needs only the stdlib, verified by import.
    # SHA256 measured 2026-09-02 by downloading it and matched against the
    # publisher's own .sha256 sidecar asset.
    'linux': {
        'python_version': '3.13.1',
        'python_url': ('https://github.com/astral-sh/python-build-standalone/releases/'
                       'download/20250115/cpython-3.13.1%2B20250115-x86_64-unknown-'
                       'linux-gnu-install_only_stripped.tar.gz'),
        'python_sha256': '56817aa976e4886bec1677699c136cb01c1cdfe0495104c0d8ef546541864bbb',
        # tinycc publishes no prebuilt Linux binary (savannah ships source
        # tarballs only), so there is no pinned tcc on this platform. The
        # bundled interpreter plus compile_overlays.py can still drive a gcc
        # from PATH, which is the common case on Linux; a player with no
        # compiler at all gets the shipped AOT cache and no autocompile, and
        # main.cpp says so on stdout rather than silently doing nothing.
        'tcc_version': None,
        'tcc_url': None,
        'tcc_sha256': None,
    },
}


def get_pinned_archive(url, sha256, destination, retries=4, log=print):
    """Fetch `url` to `destination`, verifying SHA256 on EVERY use.

    A bare `if not exists: download` trusts whatever the mirror served the day
    the download cache was first filled, forever. python.org and savannah both
    502 periodically, so transient failures retry with backoff rather than
    losing a whole release build.
    """
    name = os.path.basename(destination)
    want = sha256.lower()

    def digest(path):
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            for chunk in iter(lambda: f.read(1 << 20), b''):
                h.update(chunk)
        return h.hexdigest()

    if os.path.isfile(destination):
        have = digest(destination)
        if have == want:
            return destination
        log('release_stage: %s in the download cache has SHA256 %s (expected '
            '%s); refetching' % (name, have, want))
        os.remove(destination)

    tmp = destination + '.tmp'
    for attempt in range(1, retries + 1):
        try:
            if os.path.exists(tmp):
                os.remove(tmp)
            with urllib.request.urlopen(url, timeout=120) as r, open(tmp, 'wb') as f:
                shutil.copyfileobj(r, f)
            got = digest(tmp)
            if got != want:
                os.remove(tmp)
                raise StageError('SHA256 mismatch for %s: got %s, expected %s'
                                 % (name, got, want))
            os.replace(tmp, destination)
            return destination
        except StageError:
            raise
        except Exception as e:                                  # noqa: BLE001
            if attempt == retries:
                _die('failed to fetch %s after %d attempts: %s. Place a '
                     'verified copy at %s and re-run.'
                     % (name, retries, e, destination))
            delay = min(30, 2 ** attempt)
            log('release_stage: %s fetch attempt %d failed (%s); retrying in %ds'
                % (name, attempt, e, delay))
            time.sleep(delay)


def _extract_zip(archive, dest):
    """Extract a zip whose members sit at the archive root INTO `dest`.

    python.org's embeddable zip and tinycc's win64-bin zip are both of this
    shape (the tcc one nests a single `tcc/` directory, handled by the caller).
    """
    _mkdirs(dest)
    with zipfile.ZipFile(archive) as z:
        for m in z.namelist():
            if m.startswith('/') or '..' in m.replace('\\', '/').split('/'):
                _die('refusing to extract %s: member escapes the destination (%s)'
                     % (archive, m))
        z.extractall(dest)
    return dest


def _extract_tar_top_level(archive, parent, expect_top):
    """Extract a tarball whose every member lives under one directory named
    `expect_top`, placing that directory inside `parent`.

    python-build-standalone's install_only tarballs are exactly this shape
    (every member is `python/...`). Asserting the shape rather than guessing at
    it means a future pin whose layout changed fails here instead of producing
    an `overlay_toolchain/python` that does not contain an interpreter -- which
    the runtime would read as "no toolchain" and never mention again.
    """
    _mkdirs(parent)
    with tarfile.open(archive) as t:
        names = t.getnames()
        bad = [n for n in names
               if not (n == expect_top or n.startswith(expect_top + '/'))]
        if bad:
            _die('%s does not have a single %r top-level directory (e.g. %r); '
                 'the pinned archive layout changed'
                 % (os.path.basename(archive), expect_top, bad[0]))
        # filter='data' refuses absolute paths, `..` escapes, device nodes and
        # symlinks pointing out of the tree. Python 3.14 makes it the default;
        # being explicit keeps 3.9-3.13 from trusting the archive.
        try:
            t.extractall(parent, filter='data')
        except TypeError:
            t.extractall(parent)
    out = os.path.join(parent, expect_top)
    if not os.path.isdir(out):
        _die('extracting %s did not produce %s' % (archive, out))
    return out


# The runtime gates autocompile on the presence of the bundled interpreter at
# this exact path (runtime/src/main.cpp, `tk_py`). The name differs per
# platform; the layout does not.
TOOLCHAIN_PY_REL = {'win': os.path.join('python', 'python.exe'),
                    'linux': os.path.join('python', 'bin', 'python3')}
TOOLCHAIN_RECOMPILER = {'win': 'psxrecomp-game.exe', 'linux': 'psxrecomp-game'}


def stage_toolchain(stage, recomp_dir, recomp_tools, recomp_include, dl_cache,
                    platform_tag=None, mingw_bin=None, log=print):
    """Stage the self-contained overlay toolchain (interpreter + recompiler +
    headers, plus tcc where a prebuilt one is published).

    This is the fallback that lets a player with no compiler installed still
    turn captured overlays into native code. Without it the runtime's
    autocompile gate is false and overlays outside the shipped cache stay
    interpreted forever. No Linux packager has ever staged one (measured:
    `grep -c overlay_toolchain` = 0 in all three forked package_appimage.sh),
    so on Linux neither the AOT cache nor the capture-and-compile fail-safe
    could extend itself.
    """
    platform_tag = platform_tag or ('win' if co.is_windows() else 'linux')
    if platform_tag not in TOOLCHAIN_PINS:
        _die('no toolchain pins for platform %r' % platform_tag)
    pins = TOOLCHAIN_PINS[platform_tag]
    toolchain = _mkdirs(os.path.join(stage, 'overlay_toolchain'))
    _mkdirs(dl_cache)

    py_dest = os.path.join(toolchain, 'python')
    if os.path.isdir(py_dest):
        shutil.rmtree(py_dest)
    py_archive = get_pinned_archive(
        pins['python_url'], pins['python_sha256'],
        os.path.join(dl_cache, _url_basename(pins['python_url'])), log=log)
    if py_archive.endswith('.zip'):
        # python.org embeddable zip: members at the archive root.
        _extract_zip(py_archive, py_dest)
    else:
        # python-build-standalone: one `python/` top-level directory.
        _extract_tar_top_level(py_archive, toolchain, 'python')

    if pins['tcc_url']:
        tcc_archive = get_pinned_archive(
            pins['tcc_url'], pins['tcc_sha256'],
            os.path.join(dl_cache, _url_basename(pins['tcc_url'])), log=log)
        tcc_tmp = os.path.join(dl_cache, 'tcc_extract')
        if os.path.isdir(tcc_tmp):
            shutil.rmtree(tcc_tmp)
        _extract_zip(tcc_archive, tcc_tmp)
        inner = os.path.join(tcc_tmp, 'tcc')
        shutil.copytree(inner if os.path.isdir(inner) else tcc_tmp,
                        os.path.join(toolchain, 'tcc'), dirs_exist_ok=True)

    recompiler = TOOLCHAIN_RECOMPILER[platform_tag]
    src_recompiler = os.path.join(recomp_dir, recompiler)
    if not os.path.isfile(src_recompiler):
        _die('overlay toolchain needs the recompiler at %s; build the '
             'psxrecomp-game target first' % src_recompiler)
    shutil.copy2(src_recompiler, os.path.join(toolchain, recompiler))
    os.chmod(os.path.join(toolchain, recompiler), 0o755)

    # Windows needs the mingw runtime beside the recompiler; a Linux build links
    # against the system libstdc++ that is already present.
    if platform_tag == 'win':
        if not mingw_bin:
            _die('--mingw-bin is required when staging a Windows toolchain')
        for d in ('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'):
            shutil.copy2(os.path.join(mingw_bin, d), os.path.join(toolchain, d))

    shutil.copy2(os.path.join(recomp_tools, 'compile_overlays.py'), toolchain)
    tool_inc = _mkdirs(os.path.join(toolchain, 'include'))
    for h in os.listdir(recomp_include):
        if h.endswith(('.h', '.c.inc')):
            shutil.copy2(os.path.join(recomp_include, h), tool_inc)

    # The runtime gates autocompile on this exact file. If the layout ever
    # changes, fail here rather than shipping a toolchain the runtime ignores.
    probe = os.path.join(toolchain, TOOLCHAIN_PY_REL[platform_tag])
    if not os.path.isfile(probe):
        _die("staged overlay_toolchain is missing %s; the runtime's autocompile "
             'gate would be false and overlays outside the bundled cache would '
             'stay interpreted forever'
             % os.path.relpath(probe, toolchain))
    os.chmod(probe, 0o755)

    # lstat, and skip symlinks. os.path.getsize() follows them, and a
    # relocatable CPython is dense with them -- the staged Linux toolchain has
    # over a thousand, so following them reported ~110 MB for a tree that is
    # 80 MB on disk. A release log that overstates what it just staged by 38%
    # is a number nobody can use.
    total = 0
    links = 0
    for dirpath, _d, files in os.walk(toolchain):
        for f in files:
            p = os.path.join(dirpath, f)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if os.path.islink(p):
                links += 1
                continue
            total += st.st_size
    log('Bundled overlay toolchain (pinned python%s + recompiler + headers): '
        '~%d MB in %d file(s) + %d symlink(s)'
        % (' + tcc' if pins['tcc_url'] else '', total // (1 << 20),
           sum(len(f) for _r, _d, f in os.walk(toolchain)) - links, links))
    return toolchain


def cmd_fetch_pinned(args):
    get_pinned_archive(args.url, args.sha256, args.destination,
                       retries=args.retries)
    print(args.destination)
    return 0


def cmd_stage_toolchain(args):
    stage_toolchain(args.stage, args.recomp_dir, args.recomp_tools,
                    args.recomp_include, args.dl_cache,
                    platform_tag=args.platform, mingw_bin=args.mingw_bin)
    return 0


# ---------------------------------------------------------------------------
# stage-mods
# ---------------------------------------------------------------------------
# Every title used to assert a HARD-CODED package count -- Tomba 2 `-ne 5`,
# MegaManX6 `-lt 16`, Ape Escape `-ne 4`, and Tomba 2's AppImage packager
# `EXPECTED_MODS=8` with a `7` hand-written into its Italian variant branch.
# All of those describe shared framework content, so all of them go stale the
# moment the framework gains or loses a mod: measured 2026-09-01, Tomba 2
# demanded 5 while the true catalog was 7, which made the title unreleasable
# for a reason that had nothing to do with it.
#
# So assert the INVARIANT, and take it from the build that produced the
# binary being packaged. runtime/runtime.cmake's _psxrt_stage_mod_catalog
# writes psx_mod_catalog_<target>.txt -- the sorted, deduplicated list of
# package ids THAT BUILD staged into mods/bundled. Comparing the staged
# catalog against that file cannot go stale when a mod is added, needs no
# per-title number, and still catches the failure that actually matters: a
# package silently not shipping.
def _find_catalog_manifest(build_path, runtime_target):
    """Locate psx_mod_catalog_<target>.txt inside a build tree.

    runtime.cmake writes it into CMAKE_CURRENT_BINARY_DIR, which is the build
    root for a title that adds its runtime target from the top-level
    CMakeLists.txt but a subdirectory for one that does not. The filename
    carries the target name, so a search cannot be ambiguous; the depth bound
    keeps it from walking a whole _deps/ tree.
    """
    want = 'psx_mod_catalog_%s.txt' % runtime_target
    root_depth = build_path.rstrip('/\\').count(os.sep)
    for dirpath, dirnames, filenames in os.walk(build_path):
        if dirpath.count(os.sep) - root_depth >= 3:
            dirnames[:] = []
        dirnames[:] = [d for d in dirnames if d not in ('CMakeFiles', '_deps')]
        if want in filenames:
            return os.path.join(dirpath, want)
    return None


def stage_mods(build_path, stage, runtime_target=None, catalog_manifest=None,
               extra_sources=(), log=print):
    mods_src = os.path.join(build_path, 'mods')
    if not os.path.isdir(os.path.join(mods_src, 'bundled')):
        _die('no mod catalog staged at %s - build the runtime target first (the '
             'framework stages both its own mods/builtin/packages and the '
             "title's packages into <exe-dir>/mods/bundled at build time; see "
             'docs/MOD_PACKAGES.md and bead beads-eio.3.101)'
             % os.path.join(mods_src, 'bundled'))

    staged_mods = os.path.join(stage, 'mods')
    if os.path.isdir(staged_mods):
        shutil.rmtree(staged_mods)
    shutil.copytree(mods_src, staged_mods, symlinks=True)

    # mods/bundled is build output and ships. Two things under mods/ belong to
    # THIS MACHINE and must never reach a release: installed/ holds .psxmod
    # archives the developer installed as a player would, and state.toml is
    # their own enable/disable selection over a catalog that ships default-off.
    shutil.rmtree(os.path.join(staged_mods, 'installed'), ignore_errors=True)
    for f in ('state.toml', 'state.toml.tmp'):
        try:
            os.remove(os.path.join(staged_mods, f))
        except OSError:
            pass

    staged_pkg_dir = os.path.join(staged_mods, 'bundled')
    staged_ids = sorted(d for d in os.listdir(staged_pkg_dir)
                        if os.path.isdir(os.path.join(staged_pkg_dir, d)))
    if not staged_ids:
        _die('staged mod catalog at %s is empty' % staged_pkg_dir)

    # ---- the derived invariant -------------------------------------------
    manifest = catalog_manifest
    if manifest is None and runtime_target:
        manifest = _find_catalog_manifest(build_path, runtime_target)
    want = set()
    origin = None
    if manifest and os.path.isfile(manifest):
        with open(manifest) as f:
            want = {ln.strip() for ln in f if ln.strip()}
        origin = manifest
    for src in extra_sources:
        pkgs = os.path.join(src, 'packages')
        if os.path.isdir(pkgs):
            want |= {d for d in os.listdir(pkgs)
                     if os.path.isdir(os.path.join(pkgs, d))}
            origin = origin or 'source trees'
    if not want:
        _die('cannot verify the mod catalog: neither the build-published '
             'manifest (psx_mod_catalog_<target>.txt in %s) nor any --mod-source '
             'tree was found. Refusing to ship an unverified catalog - a count '
             'that nothing derives is exactly the assertion that goes stale.'
             % build_path)

    missing = sorted(want - set(staged_ids))
    if missing:
        _die('mod catalog is missing package(s) the build declared: %s. They '
             'exist per %s but did not reach %s, so the release would ship a '
             'Mods page the dev build does not have.'
             % (', '.join(missing), origin, staged_pkg_dir))

    fw = [i for i in staged_ids if i.startswith('psx.')]
    game = [i for i in staged_ids if not i.startswith('psx.')]
    log('Bundled mod catalog: %d package(s) = %d game-owned + %d '
        'framework-owned (verified against %s)'
        % (len(staged_ids), len(game), len(fw), origin))
    return len(staged_ids)


def cmd_stage_mods(args):
    n = stage_mods(args.build_path, args.stage,
                   runtime_target=args.runtime_target,
                   catalog_manifest=args.catalog_manifest,
                   extra_sources=[s for s in args.mod_source if s])
    if args.count_file:
        with open(args.count_file, 'w') as f:
            f.write('%d\n' % n)
    return 0


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog='release_stage.py',
        description='Shared release staging for every psxrecomp title, on every '
                    'platform. See the module docstring and bead beads-eio.3.102.')
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('cg-tag', help='print the overlay codegen cache tag')
    p.add_argument('--runtime-include', required=True)
    p.add_argument('--recompiler', required=True,
                   help='psxrecomp-game binary (asked for --overlay-config-hash)')
    p.add_argument('--game-toml', required=True,
                   help='the STAGED/packaged game.toml, never the dev one')
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--flavor', type=int, default=None,
                   help='codegen flavor baked into the runtime being packaged '
                        '(0 base, 2 pgxp). NOT platform-dependent.')
    g.add_argument('--flavor-from-build', metavar='BUILD_DIR',
                   help='read the flavor the build published for --runtime-target')
    p.add_argument('--runtime-target', default='psx-runtime',
                   help='CMake target name, for --flavor-from-build')
    p.set_defaults(func=cmd_cg_tag)

    p = sub.add_parser('stage-cache', help='stage the overlay shard cache')
    p.add_argument('--game-id', required=True)
    p.add_argument('--cache-src-root', required=True,
                   help='the cache/ root that holds <game-id>/')
    p.add_argument('--stage', required=True, help='payload staging directory')
    p.add_argument('--cg-tag', required=True)
    p.add_argument('--arch-abi', default=None,
                   help='default: this host (compile_overlays.cache_arch_abi)')
    p.add_argument('--shared-ext', default=None,
                   help='default: this host (compile_overlays.overlay_ext)')
    p.add_argument('--count-file', default=None,
                   help='write the staged shard count here (for shell callers)')
    # Named for what it does, so it can never read like a routine option in a
    # release recipe. The old `--allow-no-cache` is why a packager that staged
    # nothing looked like a successful build for two releases.
    p.add_argument('--ship-without-overlay-cache-because', metavar='REASON',
                   default=None,
                   help='deliberately ship a package whose overlays all run '
                        'interpreted; requires a written reason, which is '
                        'printed. Never use this in a release recipe.')
    p.set_defaults(func=cmd_stage_cache)

    p = sub.add_parser('fetch-pinned',
                       help='fetch an archive, verifying SHA256 on every use')
    p.add_argument('--url', required=True)
    p.add_argument('--sha256', required=True)
    p.add_argument('--destination', required=True)
    p.add_argument('--retries', type=int, default=4)
    p.set_defaults(func=cmd_fetch_pinned)

    p = sub.add_parser('stage-toolchain', help='stage the overlay toolchain')
    p.add_argument('--stage', required=True)
    p.add_argument('--recomp-dir', required=True,
                   help='directory holding the psxrecomp-game binary')
    p.add_argument('--recomp-tools', required=True, help='framework tools/')
    p.add_argument('--recomp-include', required=True, help='framework runtime/include')
    p.add_argument('--dl-cache', required=True, help='download cache directory')
    p.add_argument('--platform', choices=sorted(TOOLCHAIN_PINS), default=None)
    p.add_argument('--mingw-bin', default=None, help='Windows only')
    p.set_defaults(func=cmd_stage_toolchain)

    p = sub.add_parser('stage-mods', help='stage and verify the mod catalog')
    p.add_argument('--build-path', required=True,
                   help='directory holding the built mods/bundled tree')
    p.add_argument('--stage', required=True)
    p.add_argument('--runtime-target', default=None,
                   help='CMake target, to find psx_mod_catalog_<target>.txt')
    p.add_argument('--catalog-manifest', default=None,
                   help='explicit path to the build-published catalog manifest')
    p.add_argument('--mod-source', action='append', default=[],
                   help='additional source tree whose packages/ must all ship '
                        '(repeatable); only needed when the build publishes no '
                        'manifest')
    p.add_argument('--count-file', default=None)
    p.set_defaults(func=cmd_stage_mods)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except StageError as e:
        sys.stderr.write('\nrelease_stage: %s\n\n' % e)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
