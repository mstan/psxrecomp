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
    src/traps.c
    src/gte.cpp
    src/crc32.c
    src/cdrom.c
    src/spu.c
    src/iso_reader.cpp
    src/iso_reader_c.cpp
    src/psx_oracle_stub.c
    src/psx_oracle_cmds.c
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

# ---- Beetle PSX oracle target (MinGW, same toolchain) ----
#
# Embeds beetle-psx-libretro as a golden oracle for byte-level RAM comparison.
# Requires beetle-psx built via: cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0

set(BEETLE_LIB "${CMAKE_SOURCE_DIR}/../beetle-psx/libmednafen_psx.a")

if(NOT MSVC AND EXISTS "${BEETLE_LIB}")
    message(STATUS "Beetle PSX oracle: found ${BEETLE_LIB}")

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

    add_executable(psx-beetleoracle
        ${PSX_SHARED_SOURCES}
        src/stub_interpreter.c
        ../generated/SCPH1001_full.c
        ../generated/SCPH1001_dispatch.c
        src/beetle_psx_bridge.cpp
        ${LIBRETRO_COMMON_SOURCES}
    )

    target_include_directories(psx-beetleoracle PRIVATE
        include
        ${SDL2_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/../beetle-psx/libretro-common/include
        ${CMAKE_SOURCE_DIR}/../beetle-psx
    )

    target_compile_definitions(psx-beetleoracle PRIVATE
        PSX_NATIVE_BUILD=1
        ENABLE_BEETLE_PSX_ORACLE=1
        DEFAULT_DEBUG_PORT=4370
    )

    target_link_libraries(psx-beetleoracle PRIVATE
        "${BEETLE_LIB}"
        ${SDL2_LIBRARIES}
    )

    if(WIN32 OR MINGW)
        target_link_libraries(psx-beetleoracle PRIVATE ws2_32 -lpthread)
    endif()

    if(MINGW)
        target_link_options(psx-beetleoracle PRIVATE -Wl,--stack,67108864)
    endif()

elseif(NOT MSVC)
    message(STATUS "Beetle PSX oracle: libmednafen_psx.a not found — skipping psx-beetleoracle")
endif()

# ---- DuckStation oracle target (MSVC only) ----
#
# Embeds DuckStation as a golden oracle for byte-level RAM comparison.
# Requires DuckStation pre-built at duckstation/build/ and SDL2 MSVC libs.
# Build with: cmake -G "Visual Studio 17 2022" -A x64 ..
#             cmake --build . --target psx-dsoracle --config Release

if(MSVC)
    set(DS_ROOT "${CMAKE_SOURCE_DIR}/../duckstation")
    set(DS_BUILD "${DS_ROOT}/build")
    set(DS_PREBUILT "${DS_ROOT}/dep/prebuilt/windows-x64/lib")
    set(DS_CORE_LIB "${DS_BUILD}/src/core/Release/core.lib")

    if(EXISTS "${DS_CORE_LIB}")
        message(STATUS "DuckStation oracle: core.lib found at ${DS_CORE_LIB}")

        add_executable(psx-dsoracle
            ${PSX_SHARED_SOURCES}
            src/stub_interpreter.c
            ../generated/SCPH1001_full.c
            ../generated/SCPH1001_dispatch.c
            src/duckstation_bridge.cpp
        )

        target_include_directories(psx-dsoracle PRIVATE
            include
            ${SDL2_INCLUDE_DIRS}
            ${DS_ROOT}/src
            ${DS_ROOT}/dep
            ${DS_ROOT}/dep/fmt/include
            ${DS_ROOT}/dep/fast_float/include
            ${DS_ROOT}/dep/xxhash/include
            ${DS_ROOT}/dep/rcheevos/include
            ${DS_ROOT}/dep/rapidyaml/src
            ${DS_ROOT}/dep/rapidyaml/ext/c4core/src
            ${DS_ROOT}/dep/imgui/include
            ${DS_ROOT}/dep/vulkan/include
            ${DS_ROOT}/dep/reshadefx/include
            ${DS_BUILD}/src/scmversion
        )

        target_compile_definitions(psx-dsoracle PRIVATE
            PSX_NATIVE_BUILD=1
            ENABLE_DUCKSTATION_ORACLE=1
            DEFAULT_DEBUG_PORT=4370
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _HAS_EXCEPTIONS=0
            _UNICODE
            UNICODE
            RC_CLIENT_SUPPORTS_RAINTEGRATION=1
            SDL_MAIN_HANDLED
            CPUINFO_SHARED
            ENABLE_RECOMPILER=1
            ENABLE_MMAP_FASTMEM=1
            ENABLE_OPENGL=1
            ENABLE_VULKAN=1
            ENABLE_SDL
            FMT_EXCEPTIONS=0
            FMT_USE_RTTI=0
            FMT_CPP_LIB_FILESYSTEM=0
            XBYAK_NO_EXCEPTION=1
            XXH_STATIC_LINKING_ONLY
            C4_NO_DEBUG_BREAK
            PLUTOSVG_HAS_FREETYPE
        )

        target_compile_features(psx-dsoracle PRIVATE cxx_std_20)
        # Match DuckStation's compiler flags: no exceptions, no RTTI, no GS.
        target_compile_options(psx-dsoracle PRIVATE /utf-8 /GS- /guard:cf- /EHs-c-)
        # Override CMake's default CRT for Debug config.
        set_target_properties(psx-dsoracle PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        )

        # DuckStation libraries (built by tools/duckstation/build.sh).
        target_link_libraries(psx-dsoracle PRIVATE
            "${DS_BUILD}/src/core/Release/core.lib"
            "${DS_BUILD}/src/common/Release/common.lib"
            "${DS_BUILD}/src/util/Release/util.lib"
            "${DS_BUILD}/src/scmversion/Release/scmversion.lib"
            "${DS_BUILD}/dep/fmt/Release/fmt.lib"
            "${DS_BUILD}/dep/xxhash/Release/xxhash.lib"
            "${DS_BUILD}/dep/rapidyaml/Release/rapidyaml.lib"
            "${DS_BUILD}/dep/rcheevos/Release/rcheevos.lib"
            "${DS_BUILD}/dep/imgui/Release/imgui.lib"
            "${DS_BUILD}/dep/libchdr/Release/libchdr.lib"
            "${DS_BUILD}/dep/lzma/Release/lzma.lib"
            "${DS_BUILD}/dep/minizip/Release/minizip.lib"
            "${DS_BUILD}/dep/cubeb/Release/cubeb.lib"
            "${DS_BUILD}/dep/cubeb/speex.dir/Release/speex.lib"
            "${DS_BUILD}/dep/glad/Release/glad.lib"
            "${DS_BUILD}/dep/reshadefx/Release/reshadefx.lib"
            "${DS_BUILD}/dep/d3d12ma/Release/d3d12ma.lib"
        )

        # DuckStation prebuilt dependencies (cpuinfo, zlib, zstd, etc.).
        target_link_libraries(psx-dsoracle PRIVATE
            "${DS_PREBUILT}/cpuinfo.lib"
            "${DS_PREBUILT}/zlib.lib"
            "${DS_PREBUILT}/zstd.lib"
            "${DS_PREBUILT}/zip.lib"
            "${DS_PREBUILT}/freetype.lib"
            "${DS_PREBUILT}/harfbuzz.lib"
            "${DS_PREBUILT}/libpng16.lib"
            "${DS_PREBUILT}/jpeg.lib"
            "${DS_PREBUILT}/libwebp.lib"
            "${DS_PREBUILT}/libsharpyuv.lib"
            "${DS_PREBUILT}/soundtouch.lib"
            "${DS_PREBUILT}/shaderc_shared.lib"
            "${DS_PREBUILT}/spirv-cross-c-shared.lib"
            "${DS_PREBUILT}/discord-rpc.lib"
            "${DS_PREBUILT}/plutosvg.lib"
            "${DS_PREBUILT}/brotlidec.lib"
            "${DS_PREBUILT}/brotlicommon.lib"
        )

        # SDL2 (our display) + SDL3 (DuckStation's util.lib) + Windows system libs.
        target_link_libraries(psx-dsoracle PRIVATE
            ${SDL2_LIBRARIES}
            "${DS_PREBUILT}/SDL3.lib"
            ws2_32 winmm shlwapi advapi32 ole32 oleaut32 uuid
            dxgi d3d11 d3d12 d3dcompiler dxguid
            setupapi version imm32
            pathcch cfgmgr32 winhttp
            onecore ksuser
            delayimp
        )
        target_link_options(psx-dsoracle PRIVATE
            /DELAYLOAD:api-ms-win-core-path-l1-1-0.dll
        )

        # Large stack + suppress CRT mismatch when mixing Debug/Release configs.
        target_link_options(psx-dsoracle PRIVATE
            /STACK:67108864,67108864
            /IGNORE:4099
            /IGNORE:4286
        )

        # Copy SDL2.dll next to output.
        if(SDL2_MSVC_DIR)
            add_custom_command(TARGET psx-dsoracle POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SDL2_MSVC_DIR}/lib/x64/SDL2.dll"
                    "$<TARGET_FILE_DIR:psx-dsoracle>/SDL2.dll"
            )
        endif()

    else()
        message(STATUS "DuckStation oracle: core.lib not found — skipping psx-dsoracle")
    endif()
endif()
