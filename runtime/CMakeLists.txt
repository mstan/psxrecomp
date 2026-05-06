cmake_minimum_required(VERSION 3.20)
project(psxrecomp-runtime C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# ---- Shared sources (both native and oracle) ----

set(PSX_SHARED_SOURCES
    src/main.cpp
    src/memory.c
    src/gpu.c
    src/gpu_sw_renderer.c
    src/dma.c
    src/timers.c
    src/interrupts.c
    src/sio.c
    src/memcard.c
    src/debug_server.c
    src/dirty_ram_interp.c
    src/traps.c
    src/crash_trace.c
    src/gte.cpp
    src/crc32.c
    src/cdrom.c
    src/spu.c
    src/iso_reader.cpp
    src/iso_reader_c.cpp
    src/psx_cycles.c
    src/starvation_ring.c
    src/card_read_summary.c
    src/card_data_writes.c
)

# ---- Native target: recompiled BIOS, stub interpreter ----

add_executable(psx-runtime
    ${PSX_SHARED_SOURCES}
    src/stub_interpreter.c
    ../generated/SCPH1001_full.c
    ../generated/SCPH1001_dispatch.c
)

target_include_directories(psx-runtime PRIVATE include)
target_compile_definitions(psx-runtime PRIVATE
    PSX_NATIVE_BUILD=1
    DEFAULT_DEBUG_PORT=4370
    PSX_ENABLE_BLOCK_CYCLES=1
    $<$<CXX_COMPILER_ID:MSVC>:SDL_MAIN_HANDLED>
)

# MSVC-specific: disable /GS (deep recursion triggers false overrun)
# and commit the full 64MB stack (default 4KB commit causes guard page faults
# during deep psx_dispatch recursion).
if(MSVC)
    # /GS- : disable stack cookie (deep recursion false positive)
    # /guard:cf- : disable CFG (large dispatch table)
    # Use static CRT (/MT) instead of DLL CRT (/MD) — the release DLL CRT's
    # __report_rangecheckfailure handler triggers __fastfail on our generated
    # code's deep recursion pattern.
    target_compile_options(psx-runtime PRIVATE /GS- /guard:cf-)
    target_link_options(psx-runtime PRIVATE /STACK:67108864,67108864 /GUARD:NO)
endif()

# ---- Oracle target: R3000A interpreter (MinGW only — not built under MSVC) ----

if(NOT MSVC)
    add_executable(psx-oracle
        ${PSX_SHARED_SOURCES}
        src/psx_interpreter.c
        ../generated/SCPH1001_full.c
        ../generated/SCPH1001_dispatch.c
    )

    target_include_directories(psx-oracle PRIVATE include)
    target_compile_definitions(psx-oracle PRIVATE
        PSX_ORACLE_BUILD=1
        DEFAULT_DEBUG_PORT=4371
    )
endif()

# ---- SDL2 detection ----

if(MSVC)
    # MSVC: use pre-downloaded SDL2 development package.
    file(GLOB SDL2_MSVC_DIR "${CMAKE_SOURCE_DIR}/../sdl2-msvc/SDL2-*")
    if(SDL2_MSVC_DIR)
        set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
        set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
        message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
    else()
        message(FATAL_ERROR "SDL2 MSVC dev package not found. Run: bash tools/build_oracle.sh")
    endif()
else()
    # MinGW: use pkg-config.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()

# ---- Common dependencies ----

set(PSX_TARGETS psx-runtime)
if(NOT MSVC)
    list(APPEND PSX_TARGETS psx-oracle)
endif()

foreach(target ${PSX_TARGETS})
    target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})

    # Winsock2 for TCP debug server.
    if(WIN32 OR MINGW)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()

    # Large stack for deep call chains during BIOS execution.
    if(MINGW)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
    elseif(MSVC)
        target_link_options(${target} PRIVATE /STACK:67108864,67108864)
    endif()
endforeach()

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

    # libretro-common sources needed by beetle-psx's static lib.
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
    message(STATUS "psx-beetle: libmednafen_psx.a not found — skipping psx-beetle")
endif()

