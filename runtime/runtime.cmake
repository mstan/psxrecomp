# Shared psxrecomp-v4 runtime CMake helpers.
#
# Include this from either the framework runtime build or a sibling game
# project. Call psxrecomp_v4_add_runtime_target() after SDL2 detection has
# populated SDL2_INCLUDE_DIRS and SDL2_LIBRARIES.

if(NOT DEFINED PSXRECOMP_V4_ROOT)
    get_filename_component(PSXRECOMP_V4_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# Production build flag: -DPSX_DEBUG_TOOLS=OFF disables the TCP debug
# server, the freeze heartbeat thread, and the hot-path log functions
# (call_entry / sio_write / probe / restore_event / thread_event) that
# the recompiled C invokes on every block. Use to test whether the
# debug infrastructure itself contributes to freezes, or to ship a
# lean production binary. Visible to all targets including psx-beetle.
option(PSX_DEBUG_TOOLS "Build with TCP debug server + heartbeat + per-block recording" ON)

if(NOT SDL2_INCLUDE_DIRS OR NOT SDL2_LIBRARIES)
    if(MSVC)
        file(GLOB SDL2_MSVC_DIR "${PSXRECOMP_V4_ROOT}/../sdl2-msvc/SDL2-*")
        if(SDL2_MSVC_DIR)
            set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
            set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
            message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
        else()
            message(FATAL_ERROR "SDL2 MSVC dev package not found")
        endif()
    else()
        get_filename_component(_psxrecomp_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        find_program(_psxrecomp_pkg_config pkg-config
            HINTS "${_psxrecomp_compiler_dir}"
            NO_DEFAULT_PATH
        )
        if(_psxrecomp_pkg_config)
            set(PKG_CONFIG_EXECUTABLE "${_psxrecomp_pkg_config}" CACHE FILEPATH "pkg-config executable" FORCE)
        endif()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
endif()

set(PSXRECOMP_V4_RUNTIME_SOURCES
    ${PSXRECOMP_V4_ROOT}/runtime/src/main.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/memory.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/gpu.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/gpu_sw_renderer.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/dma.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/mdec.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/timers.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/interrupts.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/sio.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/memcard.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/debug_server.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/dirty_ram_interp.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/fntrace.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/traps.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/crash_trace.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/freeze_heartbeat.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/gte.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/crc32.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/cdrom.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/spu.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/iso_reader.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/iso_reader_c.cpp
    ${PSXRECOMP_V4_ROOT}/runtime/src/psx_cycles.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/starvation_ring.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/card_read_summary.c
    ${PSXRECOMP_V4_ROOT}/runtime/src/card_data_writes.c
    ${PSXRECOMP_V4_ROOT}/recompiler/src/config_loader.cpp
)

set(PSXRECOMP_V4_RUNTIME_INCLUDE_DIRS
    ${PSXRECOMP_V4_ROOT}/runtime/include
    ${PSXRECOMP_V4_ROOT}/recompiler/src
    ${PSXRECOMP_V4_ROOT}/recompiler/lib/fmt/include
    ${PSXRECOMP_V4_ROOT}/recompiler/lib/toml11
)

set(PSXRECOMP_V4_BIOS_GENERATED
    ${PSXRECOMP_V4_ROOT}/generated/SCPH1001_full.c
    ${PSXRECOMP_V4_ROOT}/generated/SCPH1001_dispatch.c
)

function(psxrecomp_v4_add_runtime_target target)
    set(options ORACLE)
    set(oneValueArgs
        GAME_GENERATED_FULL_C
        GAME_GENERATED_DISPATCH_C
        DEBUG_PORT
        WINDOW_TITLE
        DEFAULT_BIOS_PATH
    )
    set(multiValueArgs EXTRAS_SOURCES)
    cmake_parse_arguments(PSXRT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # DEBUG_PORT and WINDOW_TITLE were previously required cmake-time defaults.
    # game.toml's [runtime] block is now the source of truth at run time; the
    # cmake-time values only survive as fallback when --game is not passed.
    if(NOT PSXRT_DEBUG_PORT)
        set(PSXRT_DEBUG_PORT 4370)
    endif()
    if(NOT PSXRT_WINDOW_TITLE)
        set(PSXRT_WINDOW_TITLE "${target}")
    endif()
    if(NOT PSXRT_DEFAULT_BIOS_PATH)
        set(PSXRT_DEFAULT_BIOS_PATH "${PSXRECOMP_V4_ROOT}/bios/SCPH1001.BIN")
    endif()

    set(generated_sources ${PSXRECOMP_V4_BIOS_GENERATED})
    if(PSXRT_GAME_GENERATED_FULL_C)
        set_source_files_properties("${PSXRT_GAME_GENERATED_FULL_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_GENERATED_FULL_C}")
        set(has_game_dispatch TRUE)
    endif()
    if(PSXRT_GAME_GENERATED_DISPATCH_C)
        set_source_files_properties("${PSXRT_GAME_GENERATED_DISPATCH_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_GENERATED_DISPATCH_C}")
        set(has_game_dispatch TRUE)
    endif()

    if(PSXRT_ORACLE)
        set(mode_source ${PSXRECOMP_V4_ROOT}/runtime/src/psx_interpreter.c)
    else()
        set(mode_source ${PSXRECOMP_V4_ROOT}/runtime/src/stub_interpreter.c)
    endif()

    add_executable(${target}
        ${PSXRECOMP_V4_RUNTIME_SOURCES}
        ${mode_source}
        ${generated_sources}
        ${PSXRT_EXTRAS_SOURCES}
    )

    target_include_directories(${target} PRIVATE
        ${PSXRECOMP_V4_RUNTIME_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})

    target_compile_definitions(${target} PRIVATE
        DEFAULT_DEBUG_PORT=${PSXRT_DEBUG_PORT}
        PSX_DEFAULT_BIOS_PATH="${PSXRT_DEFAULT_BIOS_PATH}"
        PSX_WINDOW_TITLE="${PSXRT_WINDOW_TITLE}"
        FMT_HEADER_ONLY=1
        $<$<CXX_COMPILER_ID:MSVC>:SDL_MAIN_HANDLED>
    )

    if(PSXRT_ORACLE)
        target_compile_definitions(${target} PRIVATE PSX_ORACLE_BUILD=1)
    else()
        target_compile_definitions(${target} PRIVATE
            PSX_NATIVE_BUILD=1
            PSX_ENABLE_BLOCK_CYCLES=1
        )
    endif()
    if(has_game_dispatch)
        target_compile_definitions(${target} PRIVATE PSX_HAS_GAME_DISPATCH=1)
    endif()

    # PSX_DEBUG_TOOLS option declared at the top of runtime.cmake so it's
    # also visible to psx-beetle / non-runtime-helper targets.
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(${target} PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    if(WIN32 OR MINGW)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()

    if(MINGW)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /GS- /guard:cf-)
        target_link_options(${target} PRIVATE /STACK:67108864,67108864 /GUARD:NO)
    endif()
endfunction()
