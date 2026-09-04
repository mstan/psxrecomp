# Build-time guard over the staged mod catalog (mods/bundled).
#
# WHY THIS FILE EXISTS
# --------------------
# 4cc04be3 renamed the staged catalog directory from mods/packages to
# mods/bundled so that a rebuild could wipe build output without deleting the
# player's own installed mods. The framework's staging followed the rename and
# the shared release module (tools/release_overlay_stage.ps1, Add-ModCatalog)
# followed it. NO TITLE CMakeLists followed it, because the coupling between
# them is a `cmake -E copy_directory` DESTINATION STRING -- not a symbol, not a
# header, not a link dependency. Nothing breaks at configure time, nothing
# breaks at compile time, nothing breaks at link time. The five affected titles
# kept copying their own packages into mods/packages, where the launcher and
# the packager no longer look, and the failure surfaced only when a release
# packager ran, in a different repository, on a later day.
#
# The framework now stages both catalogs itself (see PRELOADED_MODS_DIR in
# psxrecomp_add_runtime_target), so a title never names the layout. This script
# is the tripwire that makes the remaining failure mode loud instead of silent:
#
#   * a title that still hand-rolls its own copy re-creates mods/packages, and
#   * a title whose declared packages did not reach mods/bundled at all
#
# both fail the BUILD, naming the ids and the fix, rather than producing a
# quietly incomplete Mods page that only a packager notices.
#
# USAGE
# -----
#   cmake -DPSX_MODS_DIR=<build>/mods
#         -DPSX_CATALOG_MANIFEST=<file>            # ids this target must stage
#        [-DPSX_CATALOG_ALT_MANIFESTS=<f>|<f>]     # sibling targets' id sets
#        [-DPSX_REQUIRE_STAGED=1]                  # mods/ MUST already exist
#        [-DPSX_LABEL=psx-runtime]
#         -P <framework>/runtime/psx_check_mod_catalog.cmake
#
# A manifest is one package id per line; blank lines and #-comments ignored.
# PSX_CATALOG_ALT_MANIFESTS is "|"-separated rather than ";"-separated on
# purpose: a ";" inside an add_custom_command argument is a cmake list
# separator and would silently split the -D into two arguments.
#
# PSX_REQUIRE_STAGED distinguishes the two callers. The POST_BUILD invocation
# runs immediately after staging, so "nothing is staged" is itself a defect and
# passes 1. The registered ctest may run in a tree where the runtime target was
# never built, so it passes 0 and reports a skip instead of a spurious failure.
#
# PSX_CATALOG_ALT_MANIFESTS exists for titles with two runtime targets that
# share one output directory (Tomba 2: the US psx-runtime and the Italian
# psx-runtime-ita both land in the build root and therefore share one mods/).
# Each target's POST_BUILD stages its OWN complete catalog, so after a full
# build the directory holds whichever target linked last. That is correct for
# the packager -- it builds exactly one target per variant -- but it means a
# ctest running after the whole build must accept any one target's catalog
# rather than demand a particular one.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PSX_MODS_DIR OR PSX_MODS_DIR STREQUAL "")
    message(FATAL_ERROR "psx_check_mod_catalog: PSX_MODS_DIR is required")
endif()
if(NOT DEFINED PSX_LABEL OR PSX_LABEL STREQUAL "")
    set(PSX_LABEL "runtime")
endif()
if(NOT DEFINED PSX_REQUIRE_STAGED)
    set(PSX_REQUIRE_STAGED 0)
endif()

# ---- helpers ---------------------------------------------------------------

function(_psx_read_manifest path out_var)
    set(_ids "")
    if(EXISTS "${path}")
        file(STRINGS "${path}" _lines)
        foreach(_line IN LISTS _lines)
            string(STRIP "${_line}" _line)
            if(_line STREQUAL "" OR _line MATCHES "^#")
                continue()
            endif()
            list(APPEND _ids "${_line}")
        endforeach()
    endif()
    list(REMOVE_DUPLICATES _ids)
    set(${out_var} "${_ids}" PARENT_SCOPE)
endfunction()

# Immediate subdirectory names of `root`, sorted. Empty when root is absent.
function(_psx_child_dirs root out_var)
    set(_names "")
    if(IS_DIRECTORY "${root}")
        file(GLOB _entries LIST_DIRECTORIES true "${root}/*")
        foreach(_e IN LISTS _entries)
            if(IS_DIRECTORY "${_e}")
                get_filename_component(_n "${_e}" NAME)
                list(APPEND _names "${_n}")
            endif()
        endforeach()
    endif()
    list(SORT _names)
    set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()

# TRUE when <root>/<id> holds at least one <version>/manifest.toml.
function(_psx_id_has_manifest root id out_var)
    set(_ok FALSE)
    _psx_child_dirs("${root}/${id}" _versions)
    foreach(_v IN LISTS _versions)
        if(EXISTS "${root}/${id}/${_v}/manifest.toml")
            set(_ok TRUE)
            break()
        endif()
    endforeach()
    set(${out_var} "${_ok}" PARENT_SCOPE)
endfunction()

# ---- inputs ----------------------------------------------------------------

_psx_read_manifest("${PSX_CATALOG_MANIFEST}" _expected)
list(SORT _expected)

set(_alt_sets "")
set(_all_known "${_expected}")
if(DEFINED PSX_CATALOG_ALT_MANIFESTS AND NOT PSX_CATALOG_ALT_MANIFESTS STREQUAL "")
    string(REPLACE "|" ";" _alt_manifest_list "${PSX_CATALOG_ALT_MANIFESTS}")
    foreach(_alt IN LISTS _alt_manifest_list)
        if(_alt STREQUAL "")
            continue()
        endif()
        _psx_read_manifest("${_alt}" _alt_ids)
        list(SORT _alt_ids)
        if(_alt_ids)
            # Encoded as one "|"-joined string per set: a cmake list cannot
            # nest, and these ids never contain "|".
            list(JOIN _alt_ids "|" _alt_joined)
            list(APPEND _alt_sets "${_alt_joined}")
            list(APPEND _all_known ${_alt_ids})
        endif()
    endforeach()
endif()
list(REMOVE_DUPLICATES _all_known)

set(_bundled "${PSX_MODS_DIR}/bundled")
set(_legacy  "${PSX_MODS_DIR}/packages")

# ---- 1. nothing staged at all ----------------------------------------------

if(NOT IS_DIRECTORY "${PSX_MODS_DIR}")
    if(PSX_REQUIRE_STAGED)
        message(FATAL_ERROR
            "[${PSX_LABEL}] no mod catalog was staged: ${PSX_MODS_DIR} does not "
            "exist. psxrecomp_add_runtime_target() stages the framework's "
            "mods/builtin/packages plus the title's PRELOADED_MODS_DIR into "
            "mods/bundled on every build, so an absent directory means that "
            "POST_BUILD step did not run.")
    endif()
    message(STATUS
        "[${PSX_LABEL}] mod-catalog check skipped: ${PSX_MODS_DIR} does not "
        "exist (the runtime target has not been built in this tree)")
    return()
endif()

# ---- 2. the legacy mods/packages tripwire ----------------------------------
#
# Anything the CURRENT build staged lives in mods/bundled. The staging step
# additionally removes exactly the ids it stages from any pre-existing
# mods/packages, so a stale build directory from before the split is migrated
# silently and is NOT reported here. An id reappearing under mods/packages
# after that therefore means something in THIS build wrote it there -- i.e. a
# title CMakeLists still carries its own `copy_directory ... /mods` block.
#
# Ids that are NOT part of this build are left alone and merely noted: on a
# self-compiling setup release mods/packages can legitimately hold packages the
# player installed under the pre-split layout, and the runtime's
# migrate_legacy_root() (runtime/src/mod_packages.cpp) relocates those into
# mods/installed on the next launch. Deleting them here, or failing the build
# over them, would both be wrong.

set(_strays "")
set(_foreign "")
_psx_child_dirs("${_legacy}" _legacy_ids)
foreach(_id IN LISTS _legacy_ids)
    if("${_id}" IN_LIST _all_known)
        list(APPEND _strays "${_id}")
    else()
        list(APPEND _foreign "${_id}")
    endif()
endforeach()

if(_strays)
    list(JOIN _strays "\n    " _pretty)
    message(FATAL_ERROR
        "[${PSX_LABEL}] package(s) were staged into the LEGACY mods/packages "
        "layout:\n    ${_pretty}\n"
        "  at ${_legacy}\n\n"
        "mods/packages is no longer read by the launcher or by the release "
        "packager (tools/release_overlay_stage.ps1's Add-ModCatalog reads "
        "mods/bundled and throws when a source-defined package is missing "
        "from it). A package landing there does not appear on the Mods page "
        "and does not ship.\n\n"
        "This means a CMakeLists still stages its own catalog by hand. The fix "
        "is to delete that add_custom_command and declare the directory "
        "instead, so the framework owns the layout:\n\n"
        "    psxrecomp_add_runtime_target(psx-runtime\n"
        "        ...\n"
        "        PRELOADED_MODS_DIR \"\${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded\"\n"
        "    )\n\n"
        "See docs/MOD_PACKAGES.md and bead beads-eio.3.101.")
endif()

if(_foreign)
    list(JOIN _foreign ", " _pretty_foreign)
    message(STATUS
        "[${PSX_LABEL}] note: ${_legacy} holds package(s) this build does not "
        "stage (${_pretty_foreign}). Those are pre-split player-installed "
        "packages; the runtime migrates them into mods/installed on next "
        "launch. Left untouched.")
endif()

# ---- 3. mods/bundled must exist once anything is staged --------------------

if(NOT IS_DIRECTORY "${_bundled}")
    if(PSX_REQUIRE_STAGED)
        message(FATAL_ERROR
            "[${PSX_LABEL}] ${_bundled} does not exist after staging. The "
            "framework's POST_BUILD mod-catalog step did not run or failed.")
    endif()
    message(STATUS
        "[${PSX_LABEL}] mod-catalog check skipped: ${_bundled} does not exist "
        "(the runtime target has not been built in this tree)")
    return()
endif()

_psx_child_dirs("${_bundled}" _staged)

# ---- 4. every declared id must have reached mods/bundled -------------------

set(_missing "")
set(_manifestless "")
foreach(_id IN LISTS _expected)
    if(NOT "${_id}" IN_LIST _staged)
        list(APPEND _missing "${_id}")
    else()
        _psx_id_has_manifest("${_bundled}" "${_id}" _has)
        if(NOT _has)
            list(APPEND _manifestless "${_id}")
        endif()
    endif()
endforeach()

if(_missing)
    # A sibling runtime target sharing this output directory may have staged
    # its own catalog last. Accept an exact match against any sibling set.
    list(JOIN _staged "|" _staged_joined)
    foreach(_set IN LISTS _alt_sets)
        if(_staged_joined STREQUAL "${_set}")
            string(REPLACE "|" ", " _pretty_set "${_set}")
            message(STATUS
                "[${PSX_LABEL}] mod-catalog check deferred: ${_bundled} "
                "currently holds a sibling runtime target's catalog "
                "(${_pretty_set}). Two runtime targets share one output "
                "directory here, so the last one built owns mods/bundled; each "
                "one's own POST_BUILD guard already verified it at stage time.")
            return()
        endif()
    endforeach()

    list(JOIN _missing "\n    " _pretty)
    message(FATAL_ERROR
        "[${PSX_LABEL}] declared mod package(s) never reached the staged "
        "catalog:\n    ${_pretty}\n"
        "  expected at ${_bundled}/<id>/<version>/manifest.toml\n\n"
        "The release packager asserts the same invariant and would refuse to "
        "package (Add-ModCatalog: \"Mod catalog is missing package(s) the "
        "sources define\"), so this is the same defect caught one repository "
        "and several days earlier.")
endif()

if(_manifestless)
    list(JOIN _manifestless "\n    " _pretty)
    message(FATAL_ERROR
        "[${PSX_LABEL}] staged package(s) have no <version>/manifest.toml:\n"
        "    ${_pretty}\n  under ${_bundled}\n\n"
        "A package directory without a manifest is not loadable; the launcher "
        "will silently ignore it.")
endif()

# Extra ids are reported but not fatal: mods/bundled is wiped and re-staged on
# every build, so an unexpected id is either a sibling target's package or
# something staged by a mechanism the framework does not know about. Worth
# seeing, not worth failing a build over.
set(_extra "")
foreach(_id IN LISTS _staged)
    if(NOT "${_id}" IN_LIST _expected)
        list(APPEND _extra "${_id}")
    endif()
endforeach()
if(_extra)
    list(JOIN _extra ", " _pretty)
    message(STATUS
        "[${PSX_LABEL}] note: staged catalog also holds ${_pretty} (not "
        "declared by this target)")
endif()

set(_fw "")
set(_game "")
foreach(_id IN LISTS _staged)
    if(_id MATCHES "^psx\\.")
        list(APPEND _fw "${_id}")
    else()
        list(APPEND _game "${_id}")
    endif()
endforeach()
list(LENGTH _staged _n_all)
list(LENGTH _game _n_game)
list(LENGTH _fw _n_fw)
message(STATUS
    "[${PSX_LABEL}] staged mod catalog OK: ${_n_all} package(s) = ${_n_game} "
    "game-owned + ${_n_fw} framework-owned, all under mods/bundled, no "
    "mods/packages")
