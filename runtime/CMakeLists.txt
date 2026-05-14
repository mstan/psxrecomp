cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- SDL2 detection ----

if(MSVC)
    file(GLOB SDL2_MSVC_DIR "${CMAKE_SOURCE_DIR}/../sdl2-msvc/SDL2-*")
    if(SDL2_MSVC_DIR)
        set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
        set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
        message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
    else()
        message(FATAL_ERROR "SDL2 MSVC dev package not found. Run: bash tools/build_oracle.sh")
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# ---- Native/oracle runtime targets ----

set(PSXRECOMP_V4_ROOT "${CMAKE_SOURCE_DIR}/..")
include("${CMAKE_SOURCE_DIR}/runtime.cmake")

psxrecomp_v4_add_runtime_target(psx-runtime
    DEBUG_PORT 4370
    WINDOW_TITLE "psxrecomp-v4 - BIOS"
    DEFAULT_BIOS_PATH "${PSXRECOMP_V4_ROOT}/bios/SCPH1001.BIN"
)

if(NOT MSVC)
    psxrecomp_v4_add_runtime_target(psx-oracle
        ORACLE
        DEBUG_PORT 4371
        WINDOW_TITLE "psxrecomp-v4 - Oracle"
        DEFAULT_BIOS_PATH "${PSXRECOMP_V4_ROOT}/bios/SCPH1001.BIN"
    )
endif()

# ---- psx-beetle target (Beetle PSX libretro standalone, MinGW) ----
#
# Standalone binary that runs Beetle PSX (mednafen-psx libretro core) with
# a debug server exposing the SAME wire-protocol commands as psx-runtime,
# on a different port (4380). No recomp code linked.
#
# Requires beetle-psx built via:
#   cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0

set(BEETLE_LIB "${CMAKE_SOURCE_DIR}/../beetle-psx/libmednafen_psx.a")

if(NOT MSVC AND EXISTS "${BEETLE_LIB}")
    message(STATUS "psx-beetle: found ${BEETLE_LIB}")

    set(BEETLE_DIR "${CMAKE_SOURCE_DIR}/../beetle-psx")
    set(LRC "${BEETLE_DIR}/libretro-common")
    set(LIBRETRO_COMMON_SOURCES
        ${LRC}/streams/file_stream.c
        ${LRC}/streams/file_stream_transforms.c
        ${LRC}/vfs/vfs_implementation.c
        ${LRC}/file/file_path.c
        ${LRC}/compat/compat_strl.c
        ${LRC}/compat/fopen_utf8.c
        ${LRC}/compat/compat_posix_string.c
        ${LRC}/string/stdstring.c
        ${LRC}/lists/string_list.c
        ${LRC}/encodings/encoding_utf.c
        ${LRC}/time/rtime.c
        ${LRC}/rthreads/rthreads.c
        ${LRC}/memmap/memalign.c
        ${LRC}/hash/rhash.c
    )

    add_executable(psx-beetle
        src/beetle_main.cpp
        src/beetle_libretro.cpp
        src/beetle_debug_server.c
        ${LIBRETRO_COMMON_SOURCES}
    )

    target_include_directories(psx-beetle PRIVATE
        include
        ${SDL2_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/../beetle-psx/libretro-common/include
        ${CMAKE_SOURCE_DIR}/../beetle-psx
    )

    target_compile_definitions(psx-beetle PRIVATE
        PSX_BEETLE_BUILD=1
        DEFAULT_DEBUG_PORT=4380
    )
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(psx-beetle PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    target_link_libraries(psx-beetle PRIVATE
        "${BEETLE_LIB}"
        ${SDL2_LIBRARIES}
    )

    if(WIN32 OR MINGW)
        target_link_libraries(psx-beetle PRIVATE ws2_32 -lpthread)
    endif()

    if(MINGW)
        target_link_options(psx-beetle PRIVATE -Wl,--stack,67108864)
    endif()
elseif(NOT MSVC)
    message(STATUS "psx-beetle: libmednafen_psx.a not found - skipping psx-beetle")
endif()
