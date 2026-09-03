#!/usr/bin/env python3
"""Pinning test for tools/release_stage.py's overlay-cache and mod staging.

The bug this pins (bead beads-eio.3.102) was NOT a crash and NOT a wrong
number. Three release packagers selected shards with

    find "$cache_src" -path "*/$cg_tag/*" -name '*.so'

and derived `$cg_tag` themselves. When the framework appended an `_f<flavor>`
field to the tag, two of those packagers kept emitting the shorter form. A
`-path` pattern ending in `/` requires a separator immediately after the tag,
and the real directory is named `..._f0`, so the pattern matched nothing: a
cache holding hundreds of valid shards produced `shards=0`. The packager then
exited 1 (no release at all) or, with `--allow-no-cache`, shipped an AppImage
whose every overlay dispatch ran on the dirty-RAM interpreter.

Every case below is therefore about SELECTION, not about copying:

  * a real cache tree is staged, completely, under one tag directory
  * a tag that is one field short selects NOTHING and must FAIL, not warn
  * foreign tag namespaces and foreign arch-abis are not shipped
  * .abi_<tag>.ok, .c and .pair-lock never ship, even though they sit right
    beside the shards
  * an empty selection is an error by default, and the escape hatch has to be
    asked for by name

These run against synthetic trees, so they need no recompiler, no captures and
no build -- which is the point: this is the assertion that could have run in CI
on the day the tag grew a field.
"""

import os
import shutil
import sys
import tempfile
import unittest
import zipfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.abspath(os.path.join(_HERE, '..', '..', 'tools'))
sys.path.insert(0, _TOOLS)

import release_stage as rs  # noqa: E402

GAME = 'SCUS-94454'
TAG = 'cg10_0e9a4721_gcb4cd6693_f0'
# What the two unmigrated packagers actually produced: the same tag, one field
# short. Written out here as data, not built by a format string.
TAG_SHORT = 'cg10_0e9a4721_gcb4cd6693'
ARCH = 'linux-x64'
EXT = '.so'


def touch(path, content='x'):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(content)
    return path


class CacheStagingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_relstage_')
        self.src = os.path.join(self.tmp, 'cachesrc')
        self.stage = os.path.join(self.tmp, 'stage')
        self.tagdir = os.path.join(self.src, GAME, 'gcc', ARCH, TAG)
        self.shards = []
        for i in range(5):
            self.shards.append(touch(os.path.join(self.tagdir, '8004%04x.so' % i)))
            touch(os.path.join(self.tagdir, '8004%04x.ranges' % i))
        touch(os.path.join(self.tagdir, 'bios_resident.resident'))
        self.logged = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def log(self, *a):
        self.logged.append(' '.join(str(x) for x in a))

    def stage_it(self, tag=TAG, **kw):
        return rs.stage_cache(GAME, self.src, self.stage, tag,
                              arch_abi=ARCH, shared_ext=EXT, log=self.log, **kw)

    # ---- the happy path ---------------------------------------------------
    def test_stages_every_shard_under_one_tag_dir(self):
        n, tags = self.stage_it()
        self.assertEqual(n, 5)
        self.assertEqual(tags, [TAG])
        staged = os.path.join(self.stage, 'cache', GAME, 'gcc', ARCH, TAG)
        self.assertTrue(os.path.isdir(staged))
        self.assertEqual(
            sorted(f for f in os.listdir(staged) if f.endswith(EXT)),
            sorted(os.path.basename(s) for s in self.shards))
        # The whole triple ships, not just the shared objects.
        self.assertEqual(len([f for f in os.listdir(staged)
                              if f.endswith('.ranges')]), 5)
        self.assertEqual(len([f for f in os.listdir(staged)
                              if f.endswith('.resident')]), 1)

    # ---- THE regression --------------------------------------------------
    def test_tag_missing_the_flavor_field_fails_loudly(self):
        """A tag one field short must select nothing AND stop the release."""
        with self.assertRaises(rs.StageError) as cm:
            self.stage_it(tag=TAG_SHORT)
        msg = str(cm.exception)
        self.assertIn('REFUSING TO PACKAGE', msg)
        # The message has to name the tag that WAS found, or the next person
        # debugging this learns only that "there are no shards".
        self.assertIn(TAG, msg)
        self.assertFalse(os.path.exists(os.path.join(self.stage, 'cache')),
                         'a failed selection must not leave a partial cache')

    def test_prefix_tag_does_not_match_a_longer_real_tag(self):
        """The inverse trap: a substring match would have accepted the short
        tag and silently staged shards under a directory the runtime, which
        compares the tag by NAME, would still have ignored."""
        with self.assertRaises(rs.StageError):
            self.stage_it(tag=TAG_SHORT)

    # ---- namespaces -------------------------------------------------------
    def test_foreign_tag_namespace_is_not_shipped(self):
        other = os.path.join(self.src, GAME, 'gcc', ARCH, 'cg9_deadbeef_gc12345678_f0')
        for i in range(3):
            touch(os.path.join(other, 'aa%02x.so' % i))
        n, tags = self.stage_it()
        self.assertEqual(n, 5, 'only this build\'s tag is shippable')
        self.assertEqual(tags, [TAG])

    def test_foreign_arch_abi_is_not_shipped(self):
        win = os.path.join(self.src, GAME, 'gcc', 'win-x64', TAG)
        touch(os.path.join(win, 'ff01.dll'))
        touch(os.path.join(win, 'ff01.so'))     # same tag, wrong arch
        n, _ = self.stage_it()
        self.assertEqual(n, 5)
        self.assertFalse(os.path.exists(
            os.path.join(self.stage, 'cache', GAME, 'gcc', 'win-x64')))

    def test_sljit_tier_is_not_shipped(self):
        sl = os.path.join(self.src, GAME, 'sljit', ARCH, TAG)
        touch(os.path.join(sl, 'bb01.so'))
        n, _ = self.stage_it()
        self.assertEqual(n, 5)

    # ---- artifacts that must never ship ----------------------------------
    def test_forbidden_artifacts_never_ship(self):
        touch(os.path.join(self.tagdir, '.abi_%s.ok' % TAG))
        touch(os.path.join(self.tagdir, '80040000.c'))
        touch(os.path.join(self.tagdir, '80040000.pair-lock'))
        n, _ = self.stage_it()
        self.assertEqual(n, 5)
        staged = os.path.join(self.stage, 'cache', GAME, 'gcc', ARCH, TAG)
        names = os.listdir(staged)
        self.assertFalse([f for f in names if f.startswith('.abi_')],
                         'the ABI sweep memo suppresses the runtime preflight '
                         'that rejects stale shards on first launch')
        self.assertFalse([f for f in names if f.endswith('.c')])
        self.assertFalse([f for f in names if f.endswith('.pair-lock')])

    def test_real_producer_artifacts_never_ship(self):
        """The exact set of non-shard files a REAL cache holds.

        Taken verbatim from a live Tomba 2 Linux cache build, 2026-09-02
        (184 shards, and beside them: 184 .pair-lock, 185 .c, a capacity lock,
        and a pair of in-progress temp artifacts). The temp pair is the one
        that matters, because both members END IN A SHIPPABLE SUFFIX -- a
        suffix-only filter ships them, and the shell `find -name '*.so'` these
        packagers used did exactly that.
        """
        touch(os.path.join(self.tagdir, '80040000.so.pair-lock'))
        touch(os.path.join(self.tagdir, '80040000.c'))
        touch(os.path.join(self.tagdir, '.overlay-candidate-capacity.lock'))
        touch(os.path.join(self.tagdir, '.overlay-candidate-capacity.lock.invocation'))
        touch(os.path.join(self.tagdir, '.00096000_407B1780.so.tmp.ryt4jqca.so'))
        touch(os.path.join(self.tagdir, '.00096000_407B1780.so.tmp.ryt4jqca.ranges'))
        touch(os.path.join(self.tagdir, '.abi_%s.ok' % TAG))
        n, _ = self.stage_it()
        self.assertEqual(n, 5, 'only the finished shards are shippable')
        staged = os.path.join(self.stage, 'cache', GAME, 'gcc', ARCH, TAG)
        names = sorted(os.listdir(staged))
        self.assertEqual(len(names), 11, names)   # 5 .so + 5 .ranges + 1 .resident
        self.assertFalse([f for f in names if f.startswith('.')],
                         'no shippable cache artifact is hidden')
        self.assertFalse([f for f in names if '.tmp.' in f])
        self.assertFalse([f for f in names if f.endswith(('.c', '.pair-lock'))])

    # ---- the escape hatch -------------------------------------------------
    def test_missing_cache_dir_fails_by_default(self):
        shutil.rmtree(os.path.join(self.src, GAME))
        with self.assertRaises(rs.StageError):
            self.stage_it()

    def test_escape_hatch_must_be_asked_for_by_name(self):
        shutil.rmtree(os.path.join(self.src, GAME))
        n, tags = self.stage_it(allow_empty_reason='pinning test')
        self.assertEqual((n, tags), (0, []))
        self.assertTrue(any('pinning test' in m for m in self.logged),
                        'the declared reason must be printed, so a package '
                        'shipped without a cache says so in its own log')


class ModCatalogStagingTest(unittest.TestCase):
    """The catalog is verified against the manifest the BUILD published, not
    against a per-title number. Tomba 2's AppImage packager asserted
    EXPECTED_MODS=8 for the USA variant and 7 for the Italian one; both are
    counts of shared framework content plus game content, i.e. two numbers that
    go stale independently."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_relmods_')
        self.build = os.path.join(self.tmp, 'build')
        self.stage = os.path.join(self.tmp, 'stage')
        self.ids = ['psx.enhancement.cd-speed', 'psx.enhancement.pgxp',
                    'tomba2.enhancement.widescreen']
        for i in self.ids:
            touch(os.path.join(self.build, 'mods', 'bundled', i, 'manifest.toml'),
                  'id = "%s"\n' % i)
        self.manifest = touch(
            os.path.join(self.build, 'psx_mod_catalog_psx-runtime.txt'),
            '\n'.join(sorted(self.ids)) + '\n')
        self.logged = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def log(self, *a):
        self.logged.append(' '.join(str(x) for x in a))

    def test_stages_and_verifies_against_the_build_manifest(self):
        n = rs.stage_mods(self.build, self.stage,
                          runtime_target='psx-runtime', log=self.log)
        self.assertEqual(n, 3)
        for i in self.ids:
            self.assertTrue(os.path.isfile(os.path.join(
                self.stage, 'mods', 'bundled', i, 'manifest.toml')))

    def test_a_package_the_build_declared_but_did_not_stage_fails(self):
        shutil.rmtree(os.path.join(self.build, 'mods', 'bundled',
                                   'psx.enhancement.pgxp'))
        with self.assertRaises(rs.StageError) as cm:
            rs.stage_mods(self.build, self.stage,
                          runtime_target='psx-runtime', log=self.log)
        self.assertIn('psx.enhancement.pgxp', str(cm.exception))

    def test_developer_state_never_ships(self):
        touch(os.path.join(self.build, 'mods', 'state.toml'), 'enabled = []\n')
        touch(os.path.join(self.build, 'mods', 'installed', 'mine',
                           'manifest.toml'))
        rs.stage_mods(self.build, self.stage, runtime_target='psx-runtime',
                      log=self.log)
        self.assertFalse(os.path.exists(
            os.path.join(self.stage, 'mods', 'state.toml')),
            "state.toml is the developer's own enable/disable selection over a "
            'catalog that ships default-off')
        self.assertFalse(os.path.isdir(
            os.path.join(self.stage, 'mods', 'installed')))

    def test_unverifiable_catalog_is_refused(self):
        os.remove(self.manifest)
        with self.assertRaises(rs.StageError) as cm:
            rs.stage_mods(self.build, self.stage,
                          runtime_target='psx-runtime', log=self.log)
        self.assertIn('unverified', str(cm.exception))


class FlavorTest(unittest.TestCase):
    """PSX_OVERLAY_FLAVOR is a property of the runtime BINARY (overlay_api.h;
    runtime.cmake sets 2 for a PGXP target) and is NOT platform-dependent. A
    packager must read what the build published rather than assume 0."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_relflavor_')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_reads_the_published_flavor(self):
        touch(os.path.join(self.tmp, 'psxrecomp_overlay_flavor-psx-runtime.txt'),
              '2\n')
        self.assertEqual(rs._flavor_from_build(self.tmp, 'psx-runtime'), 2)

    def test_missing_publication_fails_rather_than_defaulting(self):
        with self.assertRaises(rs.StageError) as cm:
            rs._flavor_from_build(self.tmp, 'psx-runtime')
        self.assertIn('flavor', str(cm.exception))


class CodegenHashGuardTest(unittest.TestCase):
    """A release tag must not be derived from an unbuilt runtime include tree.

    codegen_hash() legitimately falls back to 0 when overlay_codegen_hash.h is
    absent (overlay_api.h has the same __has_include fallback, so a runtime
    built the same way agrees). For a RELEASE it is always wrong: the header is
    generated by the runtime build, so a zero reads as cg<N>_00000000_... and
    names a namespace the shipped binary does not read.
    """

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_relhash_')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_unbuilt_include_tree_is_refused(self):
        with self.assertRaises(rs.StageError) as cm:
            rs._require_built_codegen_hash(self.tmp)
        msg = str(cm.exception)
        self.assertIn('overlay_codegen_hash.h', msg)
        self.assertIn('00000000', msg)

    def test_built_include_tree_passes(self):
        touch(os.path.join(self.tmp, 'overlay_codegen_hash.h'),
              '#define PSX_OVERLAY_CODEGEN_HASH 0xecd487f7\n')
        rs._require_built_codegen_hash(self.tmp)     # must not raise


class ToolchainStagingTest(unittest.TestCase):
    """The staged overlay toolchain must include every file compile_overlays.py
    needs to produce a shard, not only public C headers."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_reltool_')
        self.stage = os.path.join(self.tmp, 'stage')
        self.tools = os.path.join(self.tmp, 'tools')
        self.include = os.path.join(self.tmp, 'include')
        self.recomp = os.path.join(self.tmp, 'recompiler')
        self.mingw = os.path.join(self.tmp, 'mingw')
        self.cache = os.path.join(self.tmp, 'dl')
        for d in (self.tools, self.include, self.recomp, self.mingw):
            os.makedirs(d, exist_ok=True)
        touch(os.path.join(self.tools, 'compile_overlays.py'))
        touch(os.path.join(self.include, 'overlay_api.h'))
        touch(os.path.join(self.include, 'overlay_dispatch_preamble.c.inc'))
        touch(os.path.join(self.recomp, 'psxrecomp-game.exe'))
        for d in ('libgcc_s_seh-1.dll', 'libstdc++-6.dll',
                  'libwinpthread-1.dll'):
            touch(os.path.join(self.mingw, d))

        self.py_zip = os.path.join(self.tmp, 'python.zip')
        with zipfile.ZipFile(self.py_zip, 'w') as z:
            z.writestr('python.exe', 'fake')
        self.tcc_zip = os.path.join(self.tmp, 'tcc.zip')
        with zipfile.ZipFile(self.tcc_zip, 'w') as z:
            z.writestr('tcc/tcc.exe', 'fake')

        self.orig_pins = rs.TOOLCHAIN_PINS['win']
        self.orig_fetch = rs.get_pinned_archive
        rs.TOOLCHAIN_PINS['win'] = {
            'python_url': self.py_zip,
            'python_sha256': 'unused',
            'tcc_url': self.tcc_zip,
            'tcc_sha256': 'unused',
        }
        rs.get_pinned_archive = lambda url, _sha, _dest, log=print: url

    def tearDown(self):
        rs.TOOLCHAIN_PINS['win'] = self.orig_pins
        rs.get_pinned_archive = self.orig_fetch
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_stages_overlay_dispatch_preamble(self):
        rs.stage_toolchain(self.stage, self.recomp, self.tools, self.include,
                           self.cache, platform_tag='win',
                           mingw_bin=self.mingw, log=lambda *_a: None)
        self.assertTrue(os.path.isfile(os.path.join(
            self.stage, 'overlay_toolchain', 'include',
            'overlay_dispatch_preamble.c.inc')))


if __name__ == '__main__':
    unittest.main(verbosity=2)
