include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Offline-capable resolution of the FetchContent dependencies this tree pulls.
#
# A shipped game is configured and built on the *player's* machine, and that
# machine frequently cannot reach github.com: corporate proxy, TLS-inspecting
# firewall, or no network at all. A dependency fetched unconditionally turns
# that into a failed setup with a CMake stack trace nobody can act on, so every
# URL dependency here resolves in three steps:
#
#   1. a pre-extracted source tree — FETCHCONTENT_SOURCE_DIR_<NAME>, or the
#      dependency's documented env override — and nothing is downloaded;
#   2. a vendored archive in third_party/, verified against the same SHA256 the
#      upstream pin carries;
#   3. the upstream URL.
#
# third_party/deps.manifest is THE pin record. Both this file and
# tools/ci/vendor_deps.sh read it, so a vendored archive cannot drift from the
# URL/hash the build would otherwise have downloaded.
#
# -DPSX_DEPS_OFFLINE=ON (or PSX_DEPS_OFFLINE=1 in the environment) turns step 3
# into a hard error that names the missing archive, instead of a download that
# stalls behind a firewall and then fails deep inside a FetchContent subbuild
# where the real cause is three stack frames up.
# ---------------------------------------------------------------------------

if(NOT DEFINED PSX_THIRD_PARTY_DIR)
    get_filename_component(_psx_third_party_default
        "${CMAKE_CURRENT_LIST_DIR}/../third_party" ABSOLUTE)
    set(PSX_THIRD_PARTY_DIR "${_psx_third_party_default}" CACHE PATH
        "Directory holding vendored dependency archives (third_party/)")
    unset(_psx_third_party_default)
endif()

set(_psx_deps_offline_default OFF)
if(NOT "$ENV{PSX_DEPS_OFFLINE}" STREQUAL "" AND
   NOT "$ENV{PSX_DEPS_OFFLINE}" STREQUAL "0")
    set(_psx_deps_offline_default ON)
endif()
set(PSX_DEPS_OFFLINE "${_psx_deps_offline_default}" CACHE BOOL
    "Never download a dependency: require a vendored archive or a pre-extracted source tree")
unset(_psx_deps_offline_default)

# Look up one row of third_party/deps.manifest.
function(_psx_dependency_pin name out_file out_sha out_url)
    set(_manifest "${PSX_THIRD_PARTY_DIR}/deps.manifest")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR
            "psxrecomp: dependency manifest not found: ${_manifest}\n"
            "  Point -DPSX_THIRD_PARTY_DIR=… at the framework's third_party/ "
            "directory.")
    endif()
    file(STRINGS "${_manifest}" _lines)
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        string(REGEX MATCHALL "[^ \t]+" _fields "${_line}")
        list(LENGTH _fields _field_count)
        if(NOT _field_count EQUAL 4)
            message(FATAL_ERROR
                "psxrecomp: malformed row in ${_manifest} "
                "(want 'name file sha256 url'): ${_line}")
        endif()
        list(GET _fields 0 _row_name)
        if(_row_name STREQUAL "${name}")
            list(GET _fields 1 _row_file)
            list(GET _fields 2 _row_sha)
            list(GET _fields 3 _row_url)
            set(${out_file} "${_row_file}" PARENT_SCOPE)
            set(${out_sha}  "${_row_sha}"  PARENT_SCOPE)
            set(${out_url}  "${_row_url}"  PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "psxrecomp: no '${name}' row in ${_manifest}")
endfunction()

# psxrecomp_dependency_archive(<name> OUT_URL <var> OUT_HASH <var>
#                              [SOURCE_DIR <dir>])
#
# Sets OUT_URL to the vendored archive when third_party/ holds it, else to the
# upstream URL (fatal under PSX_DEPS_OFFLINE). OUT_HASH is the URL_HASH string
# for FetchContent_Declare and is the same either way — a vendored archive is
# verified exactly as strictly as a downloaded one.
#
# Pass the result of psxrecomp_dependency_source_dir() as SOURCE_DIR. When it is
# non-empty FetchContent uses that tree and ignores the URL, so the declaration
# still needs to be well-formed but reports nothing about a download that will
# not happen.
function(psxrecomp_dependency_archive name)
    cmake_parse_arguments(_psx_dep "" "OUT_URL;OUT_HASH;SOURCE_DIR" "" ${ARGN})
    if(NOT _psx_dep_OUT_URL OR NOT _psx_dep_OUT_HASH)
        message(FATAL_ERROR
            "psxrecomp_dependency_archive(${name}): OUT_URL and OUT_HASH are required")
    endif()
    set(_psx_dep_SILENT OFF)
    if(NOT "${_psx_dep_SOURCE_DIR}" STREQUAL "")
        set(_psx_dep_SILENT ON)
    endif()

    _psx_dependency_pin("${name}" _file _sha _url)
    set(${_psx_dep_OUT_HASH} "SHA256=${_sha}" PARENT_SCOPE)

    set(_vendored "${PSX_THIRD_PARTY_DIR}/${_file}")
    if(EXISTS "${_vendored}")
        if(NOT _psx_dep_SILENT)
            message(STATUS
                "psxrecomp: ${name} from vendored archive third_party/${_file}")
        endif()
        set(${_psx_dep_OUT_URL} "${_vendored}" PARENT_SCOPE)
        return()
    endif()

    string(TOUPPER "${name}" _uc_name)
    if(PSX_DEPS_OFFLINE)
        message(FATAL_ERROR
            "psxrecomp: PSX_DEPS_OFFLINE is ON but ${name} is not vendored.\n"
            "  expected archive: ${_vendored}\n"
            "  stage it with:    tools/ci/vendor_deps.sh ${name}\n"
            "  or point the build at an already-extracted tree with "
            "-DFETCHCONTENT_SOURCE_DIR_${_uc_name}=<dir>")
    endif()

    if(NOT _psx_dep_SILENT)
        message(STATUS
            "psxrecomp: ${name} not vendored — will download ${_url} "
            "(tools/ci/vendor_deps.sh ${name} makes this build offline-capable)")
    endif()
    set(${_psx_dep_OUT_URL} "${_url}" PARENT_SCOPE)
endfunction()

# psxrecomp_dependency_source_dir(<name> OUT <var> [ENV <var>] [MARKER <file>])
#
# Honours an already-extracted source tree, from -DFETCHCONTENT_SOURCE_DIR_<NAME>
# or from the named environment variable (which is promoted into the cache var so
# FetchContent picks it up). OUT is set to the directory, or "" when none applies.
function(psxrecomp_dependency_source_dir name)
    cmake_parse_arguments(_psx_src "" "OUT;ENV;MARKER" "" ${ARGN})
    if(NOT _psx_src_OUT)
        message(FATAL_ERROR
            "psxrecomp_dependency_source_dir(${name}): OUT is required")
    endif()
    if(NOT _psx_src_MARKER)
        set(_psx_src_MARKER "CMakeLists.txt")
    endif()

    string(TOUPPER "${name}" _uc_name)
    set(_dir "")
    if(NOT "${FETCHCONTENT_SOURCE_DIR_${_uc_name}}" STREQUAL "" AND
       EXISTS "${FETCHCONTENT_SOURCE_DIR_${_uc_name}}/${_psx_src_MARKER}")
        set(_dir "${FETCHCONTENT_SOURCE_DIR_${_uc_name}}")
    elseif(_psx_src_ENV AND NOT "$ENV{${_psx_src_ENV}}" STREQUAL "" AND
           EXISTS "$ENV{${_psx_src_ENV}}/${_psx_src_MARKER}")
        set(_dir "$ENV{${_psx_src_ENV}}")
        set(FETCHCONTENT_SOURCE_DIR_${_uc_name} "${_dir}" CACHE PATH
            "Pre-fetched ${name} source (skip download)" FORCE)
    endif()

    if(_dir)
        message(STATUS "psxrecomp: using pre-fetched ${name} source ${_dir}")
    endif()
    set(${_psx_src_OUT} "${_dir}" PARENT_SCOPE)
endfunction()
