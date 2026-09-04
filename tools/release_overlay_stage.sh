# release_overlay_stage.sh — THE shared overlay/mod release staging surface for
# every psxrecomp title's Linux packager. Source it from tools/package_appimage.sh:
#
#   . "$fw/tools/release_overlay_stage.sh"
#   psx_release_stage_init "$fw"
#   cg_tag=$(psx_overlay_cg_tag --runtime-include "$fw/runtime/include" \
#                               --recompiler "$recompiler_bin" \
#                               --game-toml "$player_toml" \
#                               --flavor-from-build "$build_dir" \
#                               --runtime-target "$runtime_target")
#   psx_add_overlay_cache     --game-id "$game_id" --cache-src-root "$cache_root" \
#                             --stage "$payload" --cg-tag "$cg_tag"
#   psx_add_overlay_toolchain --stage "$payload" --recomp-dir "$rc_dir" \
#                             --recomp-tools "$fw/tools" \
#                             --recomp-include "$fw/runtime/include" \
#                             --dl-cache "$dl_cache" --platform linux
#   psx_add_mod_catalog       --build-path "$build_dir" --stage "$payload" \
#                             --runtime-target "$runtime_target"
#
# WHY THIS FILE EXISTS, AND WHY IT IS THIS THIN
# ---------------------------------------------
# It is the sh counterpart of tools/release_overlay_stage.ps1, and like that
# module it now contains NO staging logic at all. Both are argument marshalling
# in front of tools/release_stage.py, which is the single implementation.
#
# That is deliberate and it is the fix for bead beads-eio.3.102. Before it,
# three title repos each carried a forked tools/package_appimage.sh (403/436/408
# lines) and each hand-built the overlay cache tag: a printf of codegen_ver,
# codegen_hash and overlay_config_hash into a cg<ver>_<hash>_gc<cfghash> string,
# in a heredoc that had just imported compile_overlays to get those three
# numbers -- importing the very module that owns the tag and then
# reimplementing its format string. (The literal format string is deliberately
# not reproduced anywhere outside its two owners; runtime/tests/
# test_packagers_never_format_cache_tag.py fails the build if it reappears.)
# When the framework added the `_f<flavor>` field, exactly one of
# the three forks was hand-patched to append it (ApeEscapeRecomp 4a17272). The
# other two kept emitting a tag one field short, their shard filter
# `find -path "*/$cg_tag/*"` could not match the real `..._f0/` directory, and
# they staged ZERO shards from a perfectly good cache. Tomba 2 v0.0.9 therefore
# shipped Windows-only: its packager exited 1 and there is no AppImage asset in
# that release at all.
#
# Two parallel implementations, one per platform, have to be kept in step by
# review. This bug is the proof that review does not hold: the same defect class
# had ALREADY been fixed on Windows (beads-eio.2.6, which is why cache_tag()
# exists), and Linux reproduced it anyway. So there is one implementation.
#
# RULES FOR EDITING THIS FILE
#   * No tag format string. Ever. `cache_tag()` in tools/compile_overlays.py is
#     the only place that knows the tag's shape, and release_stage.py calls it.
#   * No `find` filter that selects shards, no extension list, no arch-abi
#     string, no mod count. Those live in release_stage.py.
#   * If a function here grows a branch, the branch belongs in Python.
#   * Every function forwards "$@" verbatim, so a new option needs no change
#     here at all.
#
# There is no `--allow-no-cache` equivalent in this surface. It was the
# relaxation that let a packager which staged nothing look like a successful
# build for two releases. release_stage.py has a deliberately unwieldy
# `--ship-without-overlay-cache-because <REASON>`, which is loud, names what it
# does, prints the reason, and is not the documented path in any release recipe.

# Resolve the Python interpreter and the shared tool. Sourcing this file cannot
# succeed at all when the framework pin predates it, which is the loud failure
# we want: a title that references a framework feature it is not pinned to must
# not proceed quietly.
psx_release_stage_init() {
    _psx_fw=${1:?psx_release_stage_init needs the framework root}
    _psx_stage_tool=$_psx_fw/tools/release_stage.py
    if [ ! -f "$_psx_stage_tool" ]; then
        echo "psx_release_stage_init: $_psx_stage_tool is missing." >&2
        echo "  The pinned framework predates the shared release staging tool" >&2
        echo "  (bead beads-eio.3.102). Bump the psxrecomp submodule." >&2
        return 1
    fi
    # NEVER a bare `python`: under MSYS/Cygwin that binds to a python that
    # SIGSEGVs when spawned from a job, and the failure looks like a silent
    # fallback rather than an error.
    _psx_py=${PSX_RELEASE_STAGE_PYTHON:-python3}
    if ! command -v "$_psx_py" >/dev/null 2>&1; then
        echo "psx_release_stage_init: no '$_psx_py' on PATH (override with" \
             "PSX_RELEASE_STAGE_PYTHON)" >&2
        return 1
    fi
    return 0
}

_psx_stage() {
    if [ -z "${_psx_stage_tool:-}" ]; then
        echo "release_overlay_stage.sh: call psx_release_stage_init <framework-root> first" >&2
        return 1
    fi
    "$_psx_py" "$_psx_stage_tool" "$@"
}

# Prints the tag on stdout. Non-empty output is the caller's contract; a
# failure exits non-zero and prints nothing, so `set -e` plus a `[ -n ... ]`
# check in the caller cannot mistake one for the other.
psx_overlay_cg_tag() { _psx_stage cg-tag "$@"; }

# Stages cache/<game-id>/<tier>/<arch-abi>/<tag>/ and FAILS when it stages
# nothing. Prints the shard count line.
psx_add_overlay_cache() { _psx_stage stage-cache "$@"; }

# Stages overlay_toolchain/ (pinned relocatable interpreter + recompiler +
# headers) so the runtime's autocompile gate can be satisfied on a player's
# machine. No Linux packager has ever done this.
psx_add_overlay_toolchain() { _psx_stage stage-toolchain "$@"; }

# Stages mods/ and verifies the catalog against the manifest the BUILD
# published, so no title needs a hard-coded package count.
psx_add_mod_catalog() { _psx_stage stage-mods "$@"; }
