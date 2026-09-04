#!/usr/bin/env python3
"""No packager, on any platform, may format the overlay cache tag itself.

WHAT THIS CATCHES, AND WHY IT EXISTS
====================================
The overlay shard cache is namespaced by a tag string. Exactly two places are
allowed to know its shape:

    tools/compile_overlays.py       cache_tag() — the producer's single source
                                    of truth, called by everything else
    runtime/src/overlay_loader.c    the consumer, which must stay identical

Every third copy of that format string has cost a release. The history, all
measured:

  * beads-eio.2.6 (Windows). tools/package_release.ps1 built the tag in
    PowerShell. The framework added an `_f<flavor>` field; the PowerShell copy
    did not. The packager's filter then matched nothing, so a perfectly
    regenerated cache staged ZERO shards and the release shipped with no
    native overlays at all. Fixed by exposing cache_tag() and routing the
    packager through it.

  * beads-eio.3.102 (Linux). The identical defect, in triplicate. Three title
    repos each carried a forked tools/package_appimage.sh (403/436/408 lines)
    which imported compile_overlays and then reimplemented its format string.
    One of the three was hand-patched to append the `_f` field
    (ApeEscapeRecomp 4a17272, 2026-08-28) and the other two were not, so their
    `find -path "*/<tag>/*"` could not match the real `..._f0/` directory —
    the pattern needs a separator immediately after the tag. shards evaluated
    to 0 and the packager exited 1. Tomba 2 v0.0.9 consequently shipped
    Windows-only: there is no AppImage asset in that release at all.

Both fixes were correct and neither prevented the next occurrence, because
"keep the copies in step" is a review obligation and review does not hold
across five repositories. This test replaces the obligation with a failure.

WHY THE PATTERN IS ASSEMBLED FROM PIECES BELOW
==============================================
A test that searches for a literal string cannot contain that literal, or it
finds itself. The pattern is therefore concatenated at runtime. Do not "tidy"
it back into one string.

Scope: source and script files. A comment carrying the literal format is
flagged too, deliberately — a commented-out copy is a copy waiting to be
uncommented, and every occurrence in this repository's own documentation reads
better as `cg<ver>_<hash>_gc<cfghash>_f<flavor>` anyway.

USE FROM A TITLE REPOSITORY
===========================
This is the shared check, not a framework-only one. A title's own test suite
runs the same file against its own tree, so every repo gets the same
assertion from one implementation:

    python3 <framework>/runtime/tests/test_packagers_never_format_cache_tag.py \
        --root <title repo root>

With --root given, NOTHING in that tree may format the tag, and every
tools/package_*.sh / tools/package_*.ps1 must route through the framework's
shared staging surface.
"""

import argparse
import os
import re
import sys

# --- the forbidden patterns ------------------------------------------------
# Assembled, never written whole. See the docstring.
_CG = 'cg%' + 'd_%' + '08x_gc%' + '08x'

FORBIDDEN = [
    (re.compile(re.escape(_CG)),
     'printf-style cache tag format (the exact string that went stale twice)'),
    # A tag built by interpolation or concatenation rather than printf: a
    # shell "cg<dollar-brace ver>_..._gc<dollar-brace cfg>", a Python f-string
    # with {} substitutions, or a "cg" + ver + "_" ... + "_gc" concatenation.
    # (Spelled out in words here rather than shown, so this comment does not
    # itself match -- see the docstring.)
    # Matched only when a substitution marker sits between the two anchors, so
    # a fully literal example tag in prose (cg10_ad91f28e_gcc31ae4a9_f0) does
    # not trip it -- that is documentation of an observed value, not a second
    # implementation.
    (re.compile(r'cg[^\s"\']*[${}%][^\s"\']*_gc'),
     'cache tag assembled by interpolation/concatenation'),
]

# The only two files permitted to know the tag's shape, relative to the
# framework root.
TAG_OWNERS = (
    os.path.join('tools', 'compile_overlays.py'),
    os.path.join('runtime', 'src', 'overlay_loader.c'),
)

# This file defines the patterns, so it necessarily discusses them. It is
# exempt from the scan and from nothing else: it contains no tag format string
# (the printf one is assembled at runtime, and the positive control below
# proves the assembled pattern still matches the real owners).
SELF = os.path.join('runtime', 'tests',
                    'test_packagers_never_format_cache_tag.py')

SCANNED_SUFFIXES = ('.py', '.c', '.h', '.cpp', '.cc', '.inc', '.cmake',
                    '.sh', '.ps1', '.bat', '.psm1')
SCANNED_NAMES = ('CMakeLists.txt',)

SKIP_DIRS = {'.git', 'build', 'generated', 'node_modules', '__pycache__',
             'lib', '_deps', 'beetle-psx', 'recomp-ui', 'psxrecomp-v4',
             'ghidra', 'seeds', 'assets'}

# A release packager that touches the OVERLAY CACHE must route through the
# framework's shared staging surface rather than reimplementing it. The
# qualifier matters: the framework also carries packagers that stage only the
# mod catalog (tools/package_release.ps1, tools/package_release_macos.sh,
# tools/package_setup_host.sh -- measured 2026-09-02, none of them mentions the
# overlay cache, the tag, or the toolchain at all). Those carry their own
# hand-written mod-catalog staging, which is the SAME duplication defect one
# layer over, and they should be migrated to Add-ModCatalog/stage-mods too --
# but that is a separate change with a separate proof (macOS in particular
# cannot be built or verified here), so this test does not pretend to cover it.
PACKAGER_RE = re.compile(r'^package_.*\.(sh|ps1)$')
TAG_CONSUMER_RE = re.compile(
    r'cg_?tag|CgTag|cache_tag|overlay_cache|overlay_toolchain', re.I)
SHARED_SURFACE_RE = re.compile(r'release_overlay_stage|release_stage\.py')


def _is_scanned(name):
    return name.endswith(SCANNED_SUFFIXES) or name in SCANNED_NAMES


def _walk(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and not d.startswith('_wt-')
                       and not d.startswith('build-')]
        for name in filenames:
            if _is_scanned(name):
                yield os.path.join(dirpath, name)


def scan_root(root, owners=()):
    """Return (offenders, packagers_not_routed).

    offenders           [(relpath, lineno, why, line)]
    packagers_not_routed [relpath]
    """
    offenders = []
    packagers = []
    owner_set = {os.path.normpath(o) for o in owners}
    for path in _walk(root):
        rel = os.path.normpath(os.path.relpath(path, root))
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                text = f.read()
        except OSError:
            continue
        if rel not in owner_set:
            for lineno, line in enumerate(text.splitlines(), 1):
                for pat, why in FORBIDDEN:
                    if pat.search(line):
                        offenders.append((rel, lineno, why, line.strip()))
                        break
        if (PACKAGER_RE.match(os.path.basename(path))
                and TAG_CONSUMER_RE.search(text)
                and not SHARED_SURFACE_RE.search(text)):
            packagers.append(rel)
    return offenders, packagers


def framework_root():
    # runtime/tests/<this file> -> framework root
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        '..', '..'))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', action='append', default=[],
                    help='extra tree to check (repeatable). In an extra tree '
                         'NOTHING may format the tag.')
    args = ap.parse_args(argv)

    failures = []
    fw = framework_root()

    # ---- the framework itself --------------------------------------------
    for owner in TAG_OWNERS:
        if not os.path.isfile(os.path.join(fw, owner)):
            failures.append(
                'the tag owner %s does not exist. Either it moved (update '
                'TAG_OWNERS here and in the loader/producer comments) or this '
                'test is running against the wrong tree.' % owner)

    # Positive control. A search that finds nothing proves nothing until it is
    # shown to find something that IS there: the previous investigation of this
    # very bug reported "no cache present" from a probe that could not have
    # found a cache at all. Each owner must actually contain the pattern.
    for owner in TAG_OWNERS:
        p = os.path.join(fw, owner)
        if not os.path.isfile(p):
            continue
        with open(p, encoding='utf-8', errors='replace') as f:
            if _CG not in f.read():
                failures.append(
                    'positive control failed: %s does not contain the cache '
                    'tag format string, so this test is searching for '
                    'something that no longer exists and would pass no matter '
                    'what any packager did.' % owner)

    offenders, packagers = scan_root(fw, owners=TAG_OWNERS + (SELF,))
    for rel, lineno, why, line in offenders:
        failures.append('%s:%d formats the overlay cache tag itself (%s)\n'
                        '        %s' % (rel, lineno, why, line))
    for rel in packagers:
        failures.append('%s is a release packager but does not reference the '
                        'shared staging surface (release_overlay_stage.sh/.ps1 '
                        'or release_stage.py)' % rel)

    # ---- extra roots (title repositories) --------------------------------
    for extra in args.root:
        extra = os.path.abspath(extra)
        if not os.path.isdir(extra):
            failures.append('--root %s is not a directory' % extra)
            continue
        offenders, packagers = scan_root(extra)
        for rel, lineno, why, line in offenders:
            failures.append('%s: %s:%d formats the overlay cache tag itself '
                            '(%s)\n        %s'
                            % (extra, rel, lineno, why, line))
        for rel in packagers:
            failures.append('%s: %s is a release packager but does not '
                            'reference the framework shared staging surface'
                            % (extra, rel))

    if failures:
        sys.stderr.write(
            '\nFAIL: the overlay cache tag must be formatted in exactly two '
            'places\n' + '=' * 72 + '\n')
        for f in failures:
            sys.stderr.write('  * %s\n' % f)
        sys.stderr.write(
            '\nThe tag\'s shape belongs to compile_overlays.cache_tag() and to\n'
            'overlay_loader.c, and nothing else. A packager must ASK for it:\n'
            '\n'
            '  Linux    . "$fw/tools/release_overlay_stage.sh"\n'
            '           cg_tag=$(psx_overlay_cg_tag --runtime-include ... )\n'
            '  Windows  . "$fw\\tools\\release_overlay_stage.ps1"\n'
            '           $CgTag = Get-OverlayCgTag -RecompInc ... \n'
            '  Direct   python3 tools/release_stage.py cg-tag ...\n'
            '\n'
            'Two copies of a format string have to be kept in step by review.\n'
            'That obligation has now failed twice, in two languages, costing a\n'
            'Windows release (beads-eio.2.6) and every Linux release of two\n'
            'titles (beads-eio.3.102). Do not add a third copy.\n')
        return 1

    print('OK: the overlay cache tag is formatted only in %s'
          % ' and '.join(TAG_OWNERS))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
