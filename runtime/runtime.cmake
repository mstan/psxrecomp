# Shared psxrecomp runtime CMake helpers.
#
# Include this from either the framework runtime build or a sibling game
# project. Call psxrecomp_add_runtime_target() after SDL2 detection has
# populated SDL2_INCLUDE_DIRS and SDL2_LIBRARIES.

if(NOT DEFINED PSXRECOMP_ROOT)
    get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# Default to an optimized build. The recompiled game is a huge (~270 MB) block of
# generated C; with no CMAKE_BUILD_TYPE the compiler emits it at -O0 and the game
# runs at a small fraction of full speed (terrible framerate). A naive
# `cmake -B build` (as in the README) must NOT produce that, so default to
# Release when the user hasn't chosen a type. Single-config generators only;
# multi-config (VS/Xcode) pick per-build. Overridable with -DCMAKE_BUILD_TYPE=...
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Release/RelWithDebInfo/Debug)" FORCE)
    message(STATUS "psxrecomp: no CMAKE_BUILD_TYPE set — defaulting to Release "
                   "(optimized). Use -DCMAKE_BUILD_TYPE=RelWithDebInfo/Debug to override.")
endif()

# Content-addressed compiler cache (ccache). git branch operations (checkout /
# merge / new branch) rewrite working-tree file mtimes, which makes ninja treat
# the ~279 MB generated-C objects as stale and recompile them (~15 min) even when
# their content is byte-identical. ccache keys the object on the PREPROCESSED
# SOURCE + compiler + flags (content, not mtime), so those recompiles collapse to
# near-instant cache hits after any branch op. Completely no-op when ccache is not
# on PATH, so builds still work without it. Set once, before any target is added.
if(NOT DEFINED CMAKE_C_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}" CACHE STRING "compiler launcher")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "compiler launcher")
        message(STATUS "psxrecomp: ccache enabled (${CCACHE_PROGRAM}) — mtime-proof rebuilds")
    else()
        message(STATUS "psxrecomp: ccache not found; generated-C rebuilds after git "
                       "branch ops will be slow. Install ccache on PATH to fix.")
    endif()
endif()

# PSX_DEBUG_TOOLS: TCP debug server + heartbeat + per-block recording.
# Defaults ON for Debug/RelWithDebInfo, OFF for Release/MinSizeRel so
# a plain cmake -DCMAKE_BUILD_TYPE=Release gives a lean production binary
# with no TCP server and no debug console. Override explicitly with
# -DPSX_DEBUG_TOOLS=ON/OFF to force either way regardless of build type.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    option(PSX_DEBUG_TOOLS "Build with TCP debug server + heartbeat + per-block recording" OFF)
else()
    option(PSX_DEBUG_TOOLS "Build with TCP debug server + heartbeat + per-block recording" ON)
endif()

if(NOT SDL2_INCLUDE_DIRS OR NOT SDL2_LIBRARIES)
    if(EMSCRIPTEN)
        # SDL2 is supplied by Emscripten's maintained port rather than a host
        # pkg-config package. The actual --use-port flag is target-scoped below
        # so native targets keep their existing discovery/link behavior.
        set(SDL2_INCLUDE_DIRS "${CMAKE_SYSROOT}/include/SDL2")
        set(SDL2_LIBRARIES "__emscripten_sdl2_port__")
    elseif(MSVC)
        file(GLOB SDL2_MSVC_DIR "${PSXRECOMP_ROOT}/../sdl2-msvc/SDL2-*")
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

# PSX_STATIC_RUNTIME: produce a 100% self-contained MinGW exe.
#
# A default MinGW build dynamically imports three NON-system DLLs —
# SDL2.dll, libgcc_s_seh-1.dll, libstdc++-6.dll — which must be shipped
# next to the exe. On a user's machine that side-by-side scheme breaks
# when a different-architecture copy of one of those DLLs is found earlier
# on the DLL search path (System32, another app on PATH), producing the
# 0xc000007b STATUS_INVALID_IMAGE_FORMAT crash on launch.
#
# Linking those runtimes (and SDL2) statically removes every non-system
# import, so the exe runs from any folder with zero bundled DLLs and the
# 0xc000007b failure mode becomes structurally impossible. Default ON for
# MinGW Release/MinSizeRel (the configs used to cut releases); override
# with -DPSX_STATIC_RUNTIME=OFF to force dynamic linking.
if(MINGW AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    option(PSX_STATIC_RUNTIME "Statically link SDL2 + libgcc/libstdc++ for a self-contained exe" ON)
else()
    option(PSX_STATIC_RUNTIME "Statically link SDL2 + libgcc/libstdc++ for a self-contained exe" OFF)
endif()

# PSX_LAUNCHER: build the integrated RmlUi launcher UI (settings/disc/memcard/
# controller front-end shown before the emulator boots). Needs the vendored
# lib/RmlUi + lib/freetype submodules. ON by default; the oracle/beetle builds
# never include it. Turn OFF for a launcher-less runtime (boots straight in).
option(PSX_LAUNCHER "Build the integrated RmlUi launcher UI" ON)

# Build the vendored RmlUi + FreeType once per CMake project (idempotent — the
# rmlui_core target guards re-entry). Both are linked statically into the exe so
# the self-contained-binary guarantee (PSX_STATIC_RUNTIME) still holds.
function(psxrecomp_ensure_launcher_libs)
    if(TARGET rmlui_core)
        return()
    endif()
    if(NOT EXISTS "${PSXRECOMP_ROOT}/lib/RmlUi/CMakeLists.txt" OR
       NOT EXISTS "${PSXRECOMP_ROOT}/lib/freetype/CMakeLists.txt")
        message(FATAL_ERROR
            "PSX_LAUNCHER=ON but lib/RmlUi or lib/freetype is missing. "
            "Run: git submodule update --init --recursive")
    endif()

    # Static libs fold into the exe (keeps the self-contained binary intact).
    set(BUILD_SHARED_LIBS OFF)

    # FreeType — RmlUi's font engine. Disable every optional codec/dep so the
    # only thing we pull in is the core font rasteriser.
    set(FT_DISABLE_ZLIB     TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2    TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG      TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI   TRUE CACHE BOOL "" FORCE)
    add_subdirectory("${PSXRECOMP_ROOT}/lib/freetype"
                     "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)
    # FreeType's add_subdirectory exports the target `freetype` but not the
    # namespaced alias (that's install-only). RmlUi's soft dependency check
    # looks for Freetype::Freetype, so create it here.
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    # RmlUi — core only, FreeType font engine, no samples/tests/PCH.
    set(RMLUI_SAMPLES            OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_FONT_ENGINE        "freetype" CACHE STRING "" FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF       CACHE BOOL   "" FORCE)
    # The bundled robin_hood hash map fails its bitness detection under MinGW
    # GCC ("#error Unsupported bitness"); use std:: containers instead.
    set(RMLUI_THIRDPARTY_CONTAINERS OFF     CACHE BOOL   "" FORCE)
    add_subdirectory("${PSXRECOMP_ROOT}/lib/RmlUi"
                     "${CMAKE_BINARY_DIR}/_deps/RmlUi-build" EXCLUDE_FROM_ALL)
endfunction()

set(PSXRECOMP_RUNTIME_SOURCES
    ${PSXRECOMP_ROOT}/runtime/src/main.cpp
    ${PSXRECOMP_ROOT}/runtime/src/memory.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_sw_renderer.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_render.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_gl_renderer.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_vk_renderer.c
    ${PSXRECOMP_ROOT}/runtime/src/dma.c
    ${PSXRECOMP_ROOT}/runtime/src/mdec.c
    ${PSXRECOMP_ROOT}/runtime/src/timers.c
    ${PSXRECOMP_ROOT}/runtime/src/interrupts.c
    ${PSXRECOMP_ROOT}/runtime/src/frame_pacing.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_fiber.c
    ${PSXRECOMP_ROOT}/runtime/src/sio.c
    ${PSXRECOMP_ROOT}/runtime/src/memcard.c
    ${PSXRECOMP_ROOT}/runtime/src/debug_server.c
    ${PSXRECOMP_ROOT}/runtime/src/dirty_ram_interp.c
    ${PSXRECOMP_ROOT}/runtime/src/fntrace.c
    ${PSXRECOMP_ROOT}/runtime/src/text_xlate.cpp
    ${PSXRECOMP_ROOT}/runtime/src/parity_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/device_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/boot_state.c
    ${PSXRECOMP_ROOT}/runtime/src/bios_hle.c
    ${PSXRECOMP_ROOT}/runtime/src/savestate.c
    ${PSXRECOMP_ROOT}/runtime/src/cosim_state.c
    ${PSXRECOMP_ROOT}/runtime/src/cosim.c
    ${PSXRECOMP_ROOT}/runtime/src/traps.c
    ${PSXRECOMP_ROOT}/runtime/src/crash_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/freeze_heartbeat.c
    ${PSXRECOMP_ROOT}/runtime/src/gte.cpp
    ${PSXRECOMP_ROOT}/runtime/src/crc32.c
    ${PSXRECOMP_ROOT}/runtime/src/disc_identity.cpp
    ${PSXRECOMP_ROOT}/runtime/src/cdrom.c
    ${PSXRECOMP_ROOT}/runtime/src/spu.c
    ${PSXRECOMP_ROOT}/runtime/src/spu_shadow.c
    ${PSXRECOMP_ROOT}/runtime/src/audio_shadow.c
    ${PSXRECOMP_ROOT}/runtime/src/audio_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/color_lut.c
    ${PSXRECOMP_ROOT}/runtime/src/iso_reader.cpp
    ${PSXRECOMP_ROOT}/runtime/src/iso_reader_c.cpp
    ${PSXRECOMP_ROOT}/runtime/src/psx_cycles.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_icache.c
    ${PSXRECOMP_ROOT}/runtime/src/starvation_ring.c
    ${PSXRECOMP_ROOT}/runtime/src/latency_ring.c
    ${PSXRECOMP_ROOT}/runtime/src/card_read_summary.c
    ${PSXRECOMP_ROOT}/runtime/src/card_data_writes.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_capture.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_loader.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_compile_worker.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_sljit.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_backend.c
    ${PSXRECOMP_ROOT}/runtime/src/autocompile.c
    ${PSXRECOMP_ROOT}/runtime/src/code_provider.c
    ${PSXRECOMP_ROOT}/runtime/src/event_ring.c
    ${PSXRECOMP_ROOT}/runtime/src/game_options.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_keybinds.c
    ${PSXRECOMP_ROOT}/recompiler/src/config_loader.cpp
    ${PSXRECOMP_ROOT}/recompiler/src/ps1_exe_parser.cpp
    # Tier-2 in-process JIT backend (sljit, BSD-2-Clause). Single TU; sljit
    # auto-detects the host arch (SLJIT_CONFIG_AUTO). See lib/sljit/LICENSE.
    ${PSXRECOMP_ROOT}/lib/sljit/sljit_src/sljitLir.c
)

if(EMSCRIPTEN)
    # Browsers cannot allocate/execute arbitrary native machine code or load
    # host DLL shards. Keep the documented interpreter correctness floor and
    # replace only the unavailable Tier-2 producer with an explicit backend.
    list(REMOVE_ITEM PSXRECOMP_RUNTIME_SOURCES
        ${PSXRECOMP_ROOT}/runtime/src/gpu_gl_renderer.c
        ${PSXRECOMP_ROOT}/runtime/src/psx_fiber.c
        ${PSXRECOMP_ROOT}/runtime/src/crash_trace.c
        ${PSXRECOMP_ROOT}/runtime/src/freeze_heartbeat.c
        ${PSXRECOMP_ROOT}/runtime/src/overlay_sljit.c
        ${PSXRECOMP_ROOT}/lib/sljit/sljit_src/sljitLir.c)
    list(APPEND PSXRECOMP_RUNTIME_SOURCES
        ${PSXRECOMP_ROOT}/runtime/src/gpu_gl_renderer_unavailable.c
        ${PSXRECOMP_ROOT}/runtime/src/psx_fiber_single_context.c
        ${PSXRECOMP_ROOT}/runtime/src/crash_trace_web.c
        ${PSXRECOMP_ROOT}/runtime/src/freeze_heartbeat_web.c
        ${PSXRECOMP_ROOT}/runtime/src/web_audio_bridge.c
        ${PSXRECOMP_ROOT}/runtime/src/overlay_sljit_unavailable.c)
endif()

set(PSXRECOMP_RUNTIME_INCLUDE_DIRS
    ${PSXRECOMP_ROOT}/runtime/include
    ${PSXRECOMP_ROOT}/recompiler/src
    ${PSXRECOMP_ROOT}/recompiler/include
    ${PSXRECOMP_ROOT}/recompiler/lib/fmt/include
    ${PSXRECOMP_ROOT}/recompiler/lib/toml11
    ${PSXRECOMP_ROOT}/lib/sljit/sljit_src
)

set(PSXRECOMP_BIOS_GENERATED
    ${PSXRECOMP_ROOT}/generated/SCPH1001_full.c
    ${PSXRECOMP_ROOT}/generated/SCPH1001_dispatch.c
)

# --- BIOS generated/ staleness check (hygiene) -----------------------------------
# generated/SCPH1001_*.c is gitignored build output produced by a SEPARATE build
# (recompiler/ -> psxrecomp-bios). Editing the BIOS emitter without re-running
# tools/regen_bios.sh leaves the runtime linking a stale BIOS that no longer matches
# the emitter (this caused a 4439-vs-4406 drift). regen_bios.sh records an emitter
# fingerprint in generated/SCPH1001.emitter.sha; recompute it here and WARN on a
# mismatch so the staleness is impossible to miss. Non-fatal: a stale-but-consistent
# BIOS still builds; opt out with -DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON.
if(NOT PSXRECOMP_SKIP_BIOS_STALE_CHECK)
    find_program(_psxrt_bash NAMES bash)
    set(_psxrt_stamp "${PSXRECOMP_ROOT}/generated/SCPH1001.emitter.sha")
    if(_psxrt_bash AND EXISTS "${PSXRECOMP_ROOT}/tools/bios_emitter_fingerprint.sh")
        execute_process(
            COMMAND "${_psxrt_bash}" "${PSXRECOMP_ROOT}/tools/bios_emitter_fingerprint.sh"
            WORKING_DIRECTORY "${PSXRECOMP_ROOT}"
            OUTPUT_VARIABLE _psxrt_cur_fp OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _psxrt_fp_rc ERROR_QUIET)
        if(_psxrt_fp_rc EQUAL 0 AND _psxrt_cur_fp)
            set(_psxrt_saved_fp "")
            if(EXISTS "${_psxrt_stamp}")
                file(READ "${_psxrt_stamp}" _psxrt_saved_fp)
                string(STRIP "${_psxrt_saved_fp}" _psxrt_saved_fp)
            endif()
            if(NOT _psxrt_saved_fp STREQUAL _psxrt_cur_fp)
                message(WARNING
                    "BIOS generated/ is STALE vs the recompiler emitter "
                    "(fingerprint mismatch).\n"
                    "  Linking generated/SCPH1001_*.c that may not match the current "
                    "emitter source.\n"
                    "  Fix:  tools/regen_bios.sh   (rebuilds psxrecomp-bios + "
                    "regenerates the BIOS)\n"
                    "  (Suppress: -DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON)")
            endif()
        endif()
    endif()
endif()

function(psxrecomp_add_runtime_target target)
    set(options ORACLE COSIM)
    set(oneValueArgs
        GAME_GENERATED_FULL_C
        GAME_GENERATED_DISPATCH_C
        GAME_OVERLAY_STATIC_C
        DEBUG_PORT
        WINDOW_TITLE
        DEFAULT_BIOS_PATH
        DEFAULT_GAME_CONFIG_PATH
        EXE_NAME
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
        set(PSXRT_DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN")
    endif()
    if(NOT DEFINED PSXRT_DEFAULT_GAME_CONFIG_PATH)
        set(PSXRT_DEFAULT_GAME_CONFIG_PATH "")
    endif()

    set(generated_sources ${PSXRECOMP_BIOS_GENERATED})
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
    # Layer B: statically-compiled overlay dispatch. Inert unless a game
    # provides a generated overlays_static.c — no target sets this yet.
    if(PSXRT_GAME_OVERLAY_STATIC_C AND EXISTS "${PSXRT_GAME_OVERLAY_STATIC_C}")
        set_source_files_properties("${PSXRT_GAME_OVERLAY_STATIC_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_OVERLAY_STATIC_C}")
        set(has_overlay_dispatch TRUE)
    endif()

    if(PSXRT_ORACLE)
        set(mode_source ${PSXRECOMP_ROOT}/runtime/src/psx_interpreter.c)
    else()
        set(mode_source ${PSXRECOMP_ROOT}/runtime/src/stub_interpreter.c)
    endif()

    add_executable(${target}
        ${PSXRECOMP_RUNTIME_SOURCES}
        ${mode_source}
        ${generated_sources}
        ${PSXRT_EXTRAS_SOURCES}
    )

    # Game-specific executable name. Every title instantiates this function with
    # the same CMake target name ("psx-runtime"), so without this they ALL produce
    # an identical "psx-runtime.exe" — launching or killing one title's process by
    # name then hits another title's running instance (e.g. an X5 dev run killing a
    # concurrent Tomba 2 run in a sibling worktree). An explicit EXE_NAME wins;
    # otherwise derive a unique, filename-safe OUTPUT_NAME from the window title
    # (which is already per-game) so each title's binary is distinct
    # (MegaManX5Recomp.exe, Tomba2Recomp.exe, ...). The CMake target name stays
    # "psx-runtime", so $<TARGET_FILE...> references and the POST_BUILD asset
    # copies below are unaffected. Oracle builds get an _oracle suffix so a game
    # and its Beetle oracle don't collide either.
    if(PSXRT_EXE_NAME)
        set(_psxrt_exe_name "${PSXRT_EXE_NAME}")
    else()
        string(MAKE_C_IDENTIFIER "${PSXRT_WINDOW_TITLE}" _psxrt_exe_name)
    endif()
    if(PSXRT_ORACLE)
        set(_psxrt_exe_name "${_psxrt_exe_name}_oracle")
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${_psxrt_exe_name}")

    # ---- overlay codegen hash (auto cache key) -----------------------------
    # Hash the recompiler's codegen sources into runtime/include/overlay_codegen_hash.h
    # (gitignored) so the overlay cache path carries cg<N>_<hash>: any emitter change
    # auto-invalidates the cache instead of silently reusing a stale-but-cgN DLL (the
    # v0.3.0 black-screen). The loader (via overlay_api.h) and compile_overlays.py both
    # read the same generated PSX_OVERLAY_CODEGEN_HASH, so they never drift. Defined
    # once (shared across psx-runtime/psx-beetle); idempotent write avoids rebuilds.
    set(_codegen_hash_hdr ${PSXRECOMP_ROOT}/runtime/include/overlay_codegen_hash.h)
    if(NOT TARGET psxrecomp_codegen_hash)
        # Canonical source list shared with recompiler/CMakeLists.txt (which bakes
        # the SAME hash into psxrecomp-game for the --codegen-hash staleness guard).
        set(PSXRECOMP_CODEGEN_HASH_ROOT ${PSXRECOMP_ROOT})
        include(${PSXRECOMP_ROOT}/runtime/codegen_hash_sources.cmake)
        set(_codegen_srcs ${PSXRECOMP_CODEGEN_HASH_SRCS})
        add_custom_command(
            OUTPUT  ${_codegen_hash_hdr}
            COMMAND ${CMAKE_COMMAND} -DOUT=${_codegen_hash_hdr} "-DSRCS=${_codegen_srcs}"
                    -P ${PSXRECOMP_ROOT}/runtime/hash_codegen.cmake
            DEPENDS ${_codegen_srcs} ${PSXRECOMP_ROOT}/runtime/hash_codegen.cmake
            COMMENT "Hashing recompiler codegen -> overlay_codegen_hash.h"
            VERBATIM)
        add_custom_target(psxrecomp_codegen_hash DEPENDS ${_codegen_hash_hdr})
    endif()
    add_dependencies(${target} psxrecomp_codegen_hash)

    # Force the cg-tag CONSUMERS to recompile whenever overlay_codegen_hash.h
    # changes. overlay_api.h pulls that header via __has_include, which the
    # compiler depfile does NOT record when the header is absent at first compile —
    # so a later hash change left a STALE baked-in PSX_OVERLAY_CODEGEN_HASH in the
    # binary, making the LOADER read cg<old> while autocompile WROTE cg<new> (the
    # read≠write overlay lag/wedge class — the runtime silently ignored the freshly
    # compiled shards). An explicit OBJECT_DEPENDS makes the dependency
    # unconditional, so the runtime's cg tag can never drift from the headers /
    # autocompile again. (add_dependencies above only orders header generation; it
    # does not force object recompiles on content change.)
    set_source_files_properties(
        ${PSXRECOMP_ROOT}/runtime/src/overlay_loader.c
        ${PSXRECOMP_ROOT}/runtime/src/boot_state.c
        PROPERTIES OBJECT_DEPENDS ${_codegen_hash_hdr})

    target_include_directories(${target} PRIVATE
        ${PSXRECOMP_RUNTIME_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
    )
    # pkg-config reports SDL2_LIBRARIES as a bare name (e.g. "SDL2" -> -lSDL2);
    # add its library dirs so the linker finds it outside default paths
    # (e.g. Homebrew's /opt/homebrew/lib on macOS). Empty/harmless on MSVC.
    if(SDL2_LIBRARY_DIRS)
        target_link_directories(${target} PRIVATE ${SDL2_LIBRARY_DIRS})
    endif()
    # For a self-contained MinGW build, link SDL2 statically via pkg-config's
    # --static link line (libSDL2.a + the full Windows system-lib chain SDL2
    # needs: winmm, imm32, ole32, oleaut32, version, setupapi, dinput8, ...).
    # Otherwise link the SDL2 import lib (needs SDL2.dll at runtime).
    if(EMSCRIPTEN)
        target_compile_options(${target} PRIVATE
            "--use-port=sdl2"
            "-pthread")
        target_link_options(${target} PRIVATE
            "--use-port=sdl2"
            "-pthread"
            "-sPROXY_TO_PTHREAD=1"
            "-sPTHREAD_POOL_SIZE=2"
            "-sALLOW_MEMORY_GROWTH=1"
            "-sINITIAL_MEMORY=402653184"
            "-sMAXIMUM_MEMORY=1073741824"
            "-sSTACK_SIZE=8388608"
            "-sDEFAULT_PTHREAD_STACK_SIZE=16777216"
            "-sNO_DISABLE_EXCEPTION_CATCHING"
            "-sFORCE_FILESYSTEM=1"
            "-sEXIT_RUNTIME=0")
        target_compile_definitions(${target} PRIVATE
            PSX_WEB=1
            PSX_NO_DEBUG_TOOLS=1)
    elseif(PSX_STATIC_RUNTIME AND SDL2_STATIC_LDFLAGS)
        target_link_libraries(${target} PRIVATE ${SDL2_STATIC_LDFLAGS})
    else()
        target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})
    endif()

    # Build identity: stamp the psxrecomp commit into the binary so a crash report
    # can be correlated to an exact build (issue #1 user reports had no version).
    # Computed at configure time from the psxrecomp repo (this file's dir); empty
    # on failure (no git / not a repo) -> crash_trace.c falls back to "unknown".
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" describe --always --dirty --tags
        OUTPUT_VARIABLE PSX_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT PSX_GIT_REV)
        set(PSX_GIT_REV "unknown")
    endif()

    target_compile_definitions(${target} PRIVATE
        DEFAULT_DEBUG_PORT=${PSXRT_DEBUG_PORT}
        PSX_DEFAULT_BIOS_PATH="${PSXRT_DEFAULT_BIOS_PATH}"
        PSX_DEFAULT_GAME_CONFIG_PATH="${PSXRT_DEFAULT_GAME_CONFIG_PATH}"
        PSX_WINDOW_TITLE="${PSXRT_WINDOW_TITLE}"
        PSX_BUILD_REV="${PSX_GIT_REV}"
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
    if(has_overlay_dispatch)
        target_compile_definitions(${target} PRIVATE PSX_HAS_OVERLAY_DISPATCH=1)
    endif()

    # PSX_DEBUG_TOOLS option declared at the top of runtime.cmake so it's
    # also visible to psx-beetle / non-runtime-helper targets.
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(${target} PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    # First-divergence co-sim oracle (COSIM_ORACLE.md): the clean, deterministic build.
    # PSX_COSIM activates the cosim engine/hooks; PSX_NO_DEBUG_TOOLS strips ALL the laggy
    # diagnostic tooling (the debug server thread, per-block recording, rings) so the run
    # is single-threaded + fast + deterministic. The two instances (this + a FORCE_INTERP
    # run) are driven in cycle-lockstep by tools/cosim.py.
    if(PSXRT_COSIM)
        target_compile_definitions(${target} PRIVATE PSX_COSIM=1 PSX_NO_DEBUG_TOOLS=1)
    endif()

    # Integrated RmlUi launcher (not in the oracle build — that's headless).
    if(PSX_LAUNCHER AND NOT PSXRT_ORACLE)
        psxrecomp_ensure_launcher_libs()
        target_sources(${target} PRIVATE
            ${PSXRECOMP_ROOT}/runtime/launcher/launcher.cpp
            ${PSXRECOMP_ROOT}/runtime/launcher/stb_image_impl.cpp
            ${PSXRECOMP_ROOT}/lib/RmlUi/Backends/RmlUi_Platform_SDL.cpp
            ${PSXRECOMP_ROOT}/lib/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
        )
        # The vendored GL3 backend uses std::all_of without including
        # <algorithm>; force-include it rather than patching the pinned
        # submodule. (GCC/Clang only; MSVC pulls it in transitively.)
        if(NOT MSVC)
            set_source_files_properties(
                ${PSXRECOMP_ROOT}/lib/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
                PROPERTIES COMPILE_OPTIONS "-include;algorithm")
        endif()
        target_include_directories(${target} PRIVATE
            ${PSXRECOMP_ROOT}/lib/RmlUi/Include
            ${PSXRECOMP_ROOT}/lib/RmlUi/Backends
            ${PSXRECOMP_ROOT}/runtime/launcher
        )
        target_link_libraries(${target} PRIVATE RmlUi::Core)
        target_compile_definitions(${target} PRIVATE PSX_LAUNCHER=1)
        # Ship the launcher assets (RML/RCSS/fonts) next to the exe.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PSXRECOMP_ROOT}/runtime/launcher/assets"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying launcher assets next to ${target}")
    endif()

    if(EMSCRIPTEN)
        # SDL's browser backend owns the WebGL/canvas integration. The first
        # web milestone deliberately uses the portable software renderer.
    elseif(WIN32 OR MINGW)
        # opengl32: GL backend (gpu_gl_renderer.c). GL 1.x is exported directly
        # by opengl32; Phase 2b will load modern GL via SDL_GL_GetProcAddress.
        target_link_libraries(${target} PRIVATE ws2_32 dbghelp comdlg32 opengl32)
    else()
        find_package(OpenGL)
        if(OpenGL_FOUND)
            target_link_libraries(${target} PRIVATE OpenGL::GL)
        endif()
    endif()

    # ---- Vulkan backend (gpu_vk_renderer.c) --------------------------------
    # Vulkan is loaded ENTIRELY dynamically via SDL_Vulkan_LoadLibrary +
    # vkGetInstanceProcAddr (mirroring how the GL backend loads modern GL through
    # SDL), so there is NO link-time dependency on vulkan-1: the self-contained
    # static exe is preserved and a machine without a Vulkan ICD still launches
    # (the backend reports init failure and the runtime falls back to software).
    # We need only the Vulkan HEADERS at compile time, plus glslc to build SPIR-V.
    # Both ship with the Vulkan SDK ($VULKAN_SDK) or a Vulkan-Headers package.
    #
    # Build-EXCLUDED by default (silent WIP): the real Vulkan path compiles only
    # with -DPSX_ENABLE_VULKAN=ON. When OFF, gpu_vk_renderer.c builds as the inert
    # stub and the exe behaves exactly as the GL/software build — no SDK/glslc
    # dependency, regardless of whether $VULKAN_SDK is present on the machine.
    option(PSX_ENABLE_VULKAN "Build the experimental Vulkan renderer backend (WIP)" OFF)
    if(PSX_ENABLE_VULKAN)
    set(_vk_inc "")
    if(DEFINED ENV{VULKAN_SDK})
        if(EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
            set(_vk_inc "$ENV{VULKAN_SDK}/Include")
        elseif(EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
            set(_vk_inc "$ENV{VULKAN_SDK}/include")
        endif()
    endif()
    if(NOT _vk_inc)
        find_path(_vk_inc vulkan/vulkan.h)
    endif()
    find_program(GLSLC_EXE NAMES glslc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(_vk_inc AND GLSLC_EXE)
        message(STATUS "Vulkan backend: headers ${_vk_inc}, glslc ${GLSLC_EXE}")
        target_include_directories(${target} PRIVATE "${_vk_inc}")
        target_compile_definitions(${target} PRIVATE PSX_HAVE_VULKAN=1)
        # Compile every shader under runtime/shaders/ to SPIR-V (glslc) and embed
        # them into one generated header (vk_shaders_spv.h) of uint32_t arrays, so
        # gpu_vk_renderer.c creates shader modules with no runtime file deps.
        find_program(PSX_PYTHON NAMES python python3)
        set(_vk_shader_dir "${PSXRECOMP_ROOT}/runtime/shaders")
        file(GLOB _vk_shaders
            "${_vk_shader_dir}/*.vert" "${_vk_shader_dir}/*.frag"
            "${_vk_shader_dir}/*.comp")
        set(_vk_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_vkgen")
        set(_vk_spv_hdr "${_vk_gen_dir}/vk_shaders_spv.h")
        add_custom_command(
            OUTPUT  "${_vk_spv_hdr}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_vk_gen_dir}"
            COMMAND "${PSX_PYTHON}"
                    "${PSXRECOMP_ROOT}/tools/embed_spirv.py"
                    --glslc "${GLSLC_EXE}"
                    --out   "${_vk_spv_hdr}"
                    ${_vk_shaders}
            DEPENDS ${_vk_shaders}
                    "${PSXRECOMP_ROOT}/tools/embed_spirv.py"
            COMMENT "Compiling + embedding Vulkan SPIR-V shaders"
            VERBATIM)
        add_custom_target(${target}_vk_shaders DEPENDS "${_vk_spv_hdr}")
        add_dependencies(${target} ${target}_vk_shaders)
        target_include_directories(${target} PRIVATE "${_vk_gen_dir}")
    else()
        message(STATUS "Vulkan backend: PSX_ENABLE_VULKAN=ON but SDK headers/glslc "
                       "not found - gpu_vk_renderer.c builds as a software stub")
    endif()
    else()
        message(STATUS "Vulkan backend: disabled (PSX_ENABLE_VULKAN=OFF) - "
                       "gpu_vk_renderer.c builds as an inert stub")
    endif()

    if(MINGW)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
        # No console window in Release MinGW builds.
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-mwindows>)
        if(PSX_STATIC_RUNTIME)
            # Fold the GCC / C++ / winpthread runtimes into the exe so it
            # imports only Windows system DLLs (no libgcc_s_seh-1.dll /
            # libstdc++-6.dll dependency). Pairs with the static SDL2 link
            # above to make the exe fully self-contained.
            target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /GS- /guard:cf-)
        target_link_options(${target} PRIVATE /STACK:67108864,67108864 /GUARD:NO)
        # No console window in Release MSVC builds. /ENTRY keeps main() as
        # the entry point (not WinMain) while switching to the Windows subsystem.
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()
endfunction()

# Compatibility for early v4 game projects that used the longer helper name.
function(psxrecomp_v4_add_runtime_target target)
    psxrecomp_add_runtime_target(${target} ${ARGN})
endfunction()
