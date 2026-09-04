include_guard(GLOBAL)

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/psx_dependency_archive.cmake")

# libchdr is the small, BSD-licensed decompressor used by MAME-compatible CHD
# images. Pin the exact source archive (and its digest) so generated games do
# not silently change their disc decoder between builds.
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "Do not install libchdr dependencies" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "Use libchdr's pinned miniz" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "Use libchdr's pinned zstd" FORCE)
set(CHDR_WANT_RAW_DATA_SECTOR ON CACHE BOOL
    "Reconstruct complete 2352-byte CD sectors" FORCE)
set(CHDR_WANT_SUBCODE ON CACHE BOOL
    "Decode CHD CD subchannel payloads" FORCE)
set(CHDR_VERIFY_BLOCK_CRC ON CACHE BOOL
    "Verify decoded CHD blocks" FORCE)

set(_psx_libchdr_timestamp_args "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    list(APPEND _psx_libchdr_timestamp_args
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# This include is unconditional — there is no find_package path and no
# toolchain pack that ships libchdr — so a player building a released game with
# no route to github.com used to fail here, in a FetchContent subbuild, with no
# usable diagnosis. The pinned archive is vendored at third_party/ for exactly
# that reason; the upstream URL stays as the fallback for a stripped checkout.
psxrecomp_dependency_source_dir(psx_libchdr
    ENV PSX_LIBCHDR_SOURCE_DIR
    OUT _psx_libchdr_src)
psxrecomp_dependency_archive(psx_libchdr
    SOURCE_DIR "${_psx_libchdr_src}"
    OUT_URL _psx_libchdr_url OUT_HASH _psx_libchdr_hash)

FetchContent_Declare(psx_libchdr
    URL
        "${_psx_libchdr_url}"
    URL_HASH
        "${_psx_libchdr_hash}"
    ${_psx_libchdr_timestamp_args})
FetchContent_MakeAvailable(psx_libchdr)

unset(_psx_libchdr_src)
unset(_psx_libchdr_url)
unset(_psx_libchdr_hash)
