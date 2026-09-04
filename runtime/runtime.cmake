# Shared psxrecomp runtime CMake helpers.
#
# Include this from either the framework runtime build or a sibling game
# project. SDL3 is the default; set -DPSX_SDL_BACKEND=SDL2 for the legacy
# backend.

if(NOT DEFINED PSXRECOMP_ROOT)
    get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

include("${PSXRECOMP_ROOT}/cmake/psx_dependency_archive.cmake")
include("${PSXRECOMP_ROOT}/runtime/chd_dependency.cmake")

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
# RetComM cmake-clang-v1 packs ship bin/ccache and prepend that dir to PATH;
# also HINT RETCOMM_TOOLCHAIN_DIR for wizards that only set the env override.
if(NOT DEFINED CMAKE_C_COMPILER_LAUNCHER)
    set(_psx_ccache_hints "")
    if(DEFINED ENV{RETCOMM_TOOLCHAIN_DIR} AND NOT "$ENV{RETCOMM_TOOLCHAIN_DIR}" STREQUAL "")
        list(APPEND _psx_ccache_hints "$ENV{RETCOMM_TOOLCHAIN_DIR}/bin")
    endif()
    find_program(CCACHE_PROGRAM NAMES ccache ccache.exe HINTS ${_psx_ccache_hints})
    unset(_psx_ccache_hints)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}" CACHE STRING "compiler launcher")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "compiler launcher")
        message(STATUS "psxrecomp: ccache enabled (${CCACHE_PROGRAM}) — mtime-proof rebuilds")
    else()
        message(STATUS "psxrecomp: ccache not found; generated-C rebuilds after git "
                       "branch ops will be slow. Install ccache on PATH (or update "
                       "cmake-clang-v1) to fix.")
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

# PSX_STATIC_RUNTIME: produce a 100% self-contained MinGW exe.
#
# A default MinGW build dynamically imports three NON-system DLLs —
# SDL.dll, libgcc_s_seh-1.dll, libstdc++-6.dll — which must be shipped
# next to the exe. On a user's machine that side-by-side scheme breaks
# when a different-architecture copy of one of those DLLs is found earlier
# on the DLL search path (System32, another app on PATH), producing the
# 0xc000007b STATUS_INVALID_IMAGE_FORMAT crash on launch.
#
# Linking those runtimes (and SDL) statically removes every non-system
# import, so the exe runs from any folder with zero bundled DLLs and the
# 0xc000007b failure mode becomes structurally impossible. Default ON for
# MinGW Release/MinSizeRel (the configs used to cut releases); override
# with -DPSX_STATIC_RUNTIME=OFF to force dynamic linking.
# zlib is folded the same way (static libz / zlibstatic) so Windows CI
# hosts do not import zlib1.dll — packagers still bundle it if a PE does.
if(MINGW AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
    option(PSX_STATIC_RUNTIME "Statically link SDL + libgcc/libstdc++ for a self-contained exe" ON)
else()
    option(PSX_STATIC_RUNTIME "Statically link SDL + libgcc/libstdc++ for a self-contained exe" OFF)
endif()

# SDL backend selection. SDL3 is fetched from an integrity-pinned stable
# release when no system package is available, so a default build does not
# silently fall back to SDL2. SDL2 remains an explicit, fully supported A/B
# backend.
set(PSX_SDL_BACKEND "SDL3" CACHE STRING "SDL backend (SDL3 or SDL2)")
set_property(CACHE PSX_SDL_BACKEND PROPERTY STRINGS SDL3 SDL2)
string(TOUPPER "${PSX_SDL_BACKEND}" _psx_sdl_backend)
if(NOT _psx_sdl_backend STREQUAL "SDL3" AND
   NOT _psx_sdl_backend STREQUAL "SDL2")
    message(FATAL_ERROR
        "PSX_SDL_BACKEND must be SDL3 or SDL2 (got '${PSX_SDL_BACKEND}')")
endif()

set(PSX_SDL_INCLUDE_DIRS "")
set(PSX_SDL_LIBRARY_DIRS "")
set(PSX_SDL_LIBRARIES "")
set(PSX_SDL_STATIC_LDFLAGS "")
set(PSX_SDL3 OFF)

# Portable cmake-clang-v1 pack roots (wizard / RetComM / CI emitter fetch).
# Collect as HINTS only — never list(PREPEND CMAKE_PREFIX_PATH …): on Windows CI
# the pack is llvm-mingw while the setup host links with MSYS2 g++, and a
# global prefix puts pack lib/ on the -L path so -static-libstdc++ can pick up
# the wrong libstdc++ (codecvt / filesystem undefined refs at link).
set(_PSX_TOOLCHAIN_PREFIX_HINTS "")
foreach(_psx_tc_env IN ITEMS RETCOMM_TOOLCHAIN_DIR PSXRECOMP_TOOLCHAIN_DIR
                              BPE_TOOLCHAIN_DIR TOOLCHAIN_DIR)
    if(DEFINED ENV{${_psx_tc_env}} AND NOT "$ENV{${_psx_tc_env}}" STREQUAL "")
        list(APPEND _PSX_TOOLCHAIN_PREFIX_HINTS "$ENV{${_psx_tc_env}}")
    endif()
endforeach()
# The compiler's own location is the hint that cannot be forgotten. Callers
# that pass -DCMAKE_C_COMPILER=<toolchain>/bin/cc without also exporting one of
# the variables above — the setup wizard among them — used to leave this list
# empty, and every find_package below then fell through to the host's copy of a
# dependency. That is not merely a different copy: cmake-clang-v1 compiles
# against its own sysroot and never searches /usr/include, so a host package is
# found, reported as "using prebuilt/system", and then fails to compile.
if(CMAKE_C_COMPILER)
    get_filename_component(_psx_cc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_psx_cc_pfx "${_psx_cc_bin}" DIRECTORY)
    if(_psx_cc_pfx AND EXISTS "${_psx_cc_pfx}")
        list(APPEND _PSX_TOOLCHAIN_PREFIX_HINTS "${_psx_cc_pfx}")
    endif()
    unset(_psx_cc_bin)
    unset(_psx_cc_pfx)
endif()
list(REMOVE_DUPLICATES _PSX_TOOLCHAIN_PREFIX_HINTS)

# A dependency found outside the compiler's sysroot may still be unusable: the
# toolchain will not search it, and for headers under /usr/include CMake cannot
# even pass -I, because it drops that directory from include lists to avoid
# disturbing the system header order. So a "found" package has to be compiled
# before it is believed. Sets <out> to TRUE/FALSE.
include(CheckIncludeFile)
# INCLUDES for a bare header directory, LIBRARIES for an imported target — an
# imported target must be asked through CMAKE_REQUIRED_LIBRARIES rather than by
# reading INTERFACE_INCLUDE_DIRECTORIES off it, because a package may carry its
# headers on a transitive target. SDL3::SDL3 does exactly that (its include
# dirs live on SDL3::Headers), so reading the property yields NOTFOUND and the
# check would fail against a perfectly good SDL3.
function(_psx_header_compiles out header)
    cmake_parse_arguments(_psx_hc "" "" "INCLUDES;LIBRARIES" ${ARGN})
    set(CMAKE_REQUIRED_INCLUDES ${_psx_hc_INCLUDES})
    set(CMAKE_REQUIRED_LIBRARIES ${_psx_hc_LIBRARIES})
    set(CMAKE_REQUIRED_QUIET ON)
    string(MAKE_C_IDENTIFIER
           "_psx_have_${header}_${_psx_hc_INCLUDES}_${_psx_hc_LIBRARIES}"
           _cache_var)
    check_include_file("${header}" ${_cache_var})
    if(${_cache_var})
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

if(_psx_sdl_backend STREQUAL "SDL3")
    set(PSX_SDL3 ON)
    option(PSX_SDL3_FETCH
        "Fetch the pinned SDL3 release when no system SDL3 package is found"
        ON)
    # cmake-clang-v1 1.0.9+ ships SDL3 under deps/; 1.0.7–1.0.8 used pack root.
    # Prefer SDL3_DIR / HINTS — never CMAKE_PREFIX_PATH=pack (mingw include poisons libc++).
    if(NOT SDL3_DIR)
        foreach(_psx_tc_pfx IN LISTS _PSX_TOOLCHAIN_PREFIX_HINTS)
            foreach(_psx_sdl_root IN ITEMS "${_psx_tc_pfx}/deps" "${_psx_tc_pfx}")
                if(EXISTS "${_psx_sdl_root}/lib/cmake/SDL3/SDL3Config.cmake" OR
                   EXISTS "${_psx_sdl_root}/lib/cmake/SDL3/SDL3-config.cmake")
                    set(SDL3_DIR "${_psx_sdl_root}/lib/cmake/SDL3")
                    break()
                endif()
            endforeach()
            if(SDL3_DIR)
                break()
            endif()
        endforeach()
    endif()
    set(_PSX_SDL3_HINTS "")
    foreach(_psx_tc_pfx IN LISTS _PSX_TOOLCHAIN_PREFIX_HINTS)
        list(APPEND _PSX_SDL3_HINTS "${_psx_tc_pfx}/deps" "${_psx_tc_pfx}")
    endforeach()
    find_package(SDL3 3.4 CONFIG QUIET COMPONENTS SDL3
        HINTS ${_PSX_SDL3_HINTS}
        PATH_SUFFIXES lib/cmake/SDL3)
    unset(_PSX_SDL3_HINTS)
    if(TARGET SDL3::SDL3)
        # Trust it only if the compiler can actually read its headers. A host
        # SDL3 under /usr/include satisfies find_package and then fails every
        # translation unit that includes psx_sdl.h.
        _psx_header_compiles(_psx_sdl3_ok "SDL3/SDL.h" LIBRARIES SDL3::SDL3)
        if(NOT _psx_sdl3_ok)
            message(FATAL_ERROR
                "psxrecomp: SDL3 was found at ${SDL3_DIR} but <SDL3/SDL.h> does "
                "not compile with ${CMAKE_C_COMPILER}.\n"
                "That compiler searches its own sysroot, not the host's "
                "/usr/include, so this SDL3 cannot be used even though CMake "
                "located it.\n"
                "Fix: point SDL3_DIR at the copy shipped with the toolchain, "
                "e.g. -DSDL3_DIR=<toolchain>/deps/lib/cmake/SDL3, or configure "
                "with -DPSX_SDL_BACKEND=SDL2, or build with a compiler that "
                "sees the host headers.")
        endif()
        unset(_psx_sdl3_ok)
        message(STATUS "psxrecomp: using prebuilt/system SDL3 (skip FetchContent)")
    endif()
    if(NOT TARGET SDL3::SDL3 AND PSX_SDL3_FETCH)
        include(FetchContent)
        # The fetched dependency is private to this build, so link it directly
        # instead of producing an SDL3 DLL that would need platform-specific
        # staging beside every generated game executable.
        set(SDL_SHARED OFF CACHE BOOL "Build SDL3 shared library" FORCE)
        set(SDL_STATIC ON CACHE BOOL "Build SDL3 static library" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL3 test library" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "Build SDL3 tests" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "Build SDL3 examples" FORCE)
        set(_psx_sdl3_timestamp_args "")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
            list(APPEND _psx_sdl3_timestamp_args
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        endif()
        # CI (tools/ci/prefetch_sdl3.sh) pre-extracts with curl --http1.1 to
        # avoid intermittent GitHub HTTP/2 REFUSED_STREAM failures from
        # CMake's file(DOWNLOAD). Prefer that tree when present.
        psxrecomp_dependency_source_dir(SDL3
            ENV PSX_SDL3_SOURCE_DIR
            OUT _psx_sdl3_src)
        # Pin (and any vendored archive) come from third_party/deps.manifest, so
        # an air-gapped tree staged by tools/ci/vendor_deps.sh needs no download.
        psxrecomp_dependency_archive(SDL3
            SOURCE_DIR "${_psx_sdl3_src}"
            OUT_URL _psx_sdl3_url OUT_HASH _psx_sdl3_hash)
        FetchContent_Declare(SDL3
            URL
                "${_psx_sdl3_url}"
            URL_HASH
                "${_psx_sdl3_hash}"
            ${_psx_sdl3_timestamp_args})
        FetchContent_MakeAvailable(SDL3)
        unset(_psx_sdl3_src)
        unset(_psx_sdl3_url)
        unset(_psx_sdl3_hash)
    endif()
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR
            "SDL3 3.4+ was not found. Install SDL3, provide SDL3_DIR, or "
            "configure with -DPSX_SDL3_FETCH=ON.")
    endif()
    if(PSX_STATIC_RUNTIME AND TARGET SDL3::SDL3-static)
        set(PSX_SDL_LIBRARIES SDL3::SDL3-static)
    else()
        set(PSX_SDL_LIBRARIES SDL3::SDL3)
    endif()
    message(STATUS "psxrecomp: SDL backend = SDL3")
else()
    if(NOT SDL2_INCLUDE_DIRS OR NOT SDL2_LIBRARIES)
        if(MSVC)
            file(GLOB SDL2_MSVC_DIR "${PSXRECOMP_ROOT}/../sdl2-msvc/SDL2-*")
            if(SDL2_MSVC_DIR)
                set(SDL2_INCLUDE_DIRS "${SDL2_MSVC_DIR}/include")
                set(SDL2_LIBRARIES "${SDL2_MSVC_DIR}/lib/x64/SDL2.lib")
                message(STATUS "SDL2 MSVC: ${SDL2_MSVC_DIR}")
            else()
                message(FATAL_ERROR "SDL2 MSVC dev package not found")
            endif()
        else()
            get_filename_component(
                _psxrecomp_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
            find_program(_psxrecomp_pkg_config pkg-config
                HINTS "${_psxrecomp_compiler_dir}"
                NO_DEFAULT_PATH)
            if(_psxrecomp_pkg_config)
                set(PKG_CONFIG_EXECUTABLE "${_psxrecomp_pkg_config}"
                    CACHE FILEPATH "pkg-config executable" FORCE)
            endif()
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(SDL2 REQUIRED sdl2)
        endif()
    endif()
    set(PSX_SDL_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}")
    set(PSX_SDL_LIBRARY_DIRS "${SDL2_LIBRARY_DIRS}")
    set(PSX_SDL_LIBRARIES "${SDL2_LIBRARIES}")
    set(PSX_SDL_STATIC_LDFLAGS "${SDL2_STATIC_LDFLAGS}")
    message(STATUS "psxrecomp: SDL backend = SDL2 (explicit fallback)")
endif()

# PSX_RECOMP_UI: wire the shared Dear ImGui launcher from the *game* repo's
# root recomp-ui submodule (CMAKE_SOURCE_DIR/recomp-ui). Not vendored in
# psxrecomp — games that need the launcher own the pin.
option(PSX_RECOMP_UI "Build the shared recomp-ui Dear ImGui launcher" ON)
option(PSX_SHELLWIN_INTERP "Default the shell-window dirty-RAM interpreter to ON ( BIOS without shell seeds )" OFF)
set(RECOMP_UI_ROOT "" CACHE PATH
    "Path to recomp-ui; empty = <game>/recomp-ui")
if(PSX_RECOMP_UI AND (NOT RECOMP_UI_ROOT OR RECOMP_UI_ROOT STREQUAL ""))
    if(EXISTS "${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake")
        set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/recomp-ui" CACHE PATH
            "Path to recomp-ui; empty = <game>/recomp-ui" FORCE)
    endif()
endif()

set(PSXRECOMP_RUNTIME_SOURCES
    ${PSXRECOMP_ROOT}/runtime/src/main.cpp
    ${PSXRECOMP_ROOT}/runtime/src/psx_window_icon.cpp
    ${PSXRECOMP_ROOT}/runtime/src/psx_sdl_audio.cpp
    ${PSXRECOMP_ROOT}/runtime/src/psx_stick.c
    ${PSXRECOMP_ROOT}/runtime/src/memory.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu.c
    ${PSXRECOMP_ROOT}/runtime/src/ws_ui_group.c
    ${PSXRECOMP_ROOT}/runtime/src/ws_aspect_cone_math.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_sw_renderer.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_vram_dirty.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_render.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_gl_renderer.c
    ${PSXRECOMP_ROOT}/runtime/src/gpu_vk_renderer.c
    ${PSXRECOMP_ROOT}/runtime/src/dma_gpu_ll.c
    ${PSXRECOMP_ROOT}/runtime/src/dma.c
    ${PSXRECOMP_ROOT}/runtime/src/mdec.c
    ${PSXRECOMP_ROOT}/runtime/src/timers.c
    ${PSXRECOMP_ROOT}/runtime/src/interrupts.c
    ${PSXRECOMP_ROOT}/runtime/src/frame_pacing.c
    ${PSXRECOMP_ROOT}/runtime/src/frame_interpolation.c
    ${PSXRECOMP_ROOT}/runtime/src/host_time.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_fiber.c
    ${PSXRECOMP_ROOT}/runtime/src/sio.c
    ${PSXRECOMP_ROOT}/runtime/src/memcard.c
    ${PSXRECOMP_ROOT}/runtime/src/debug_server.c
    ${PSXRECOMP_ROOT}/runtime/src/debug_trace_ranges.c
    ${PSXRECOMP_ROOT}/runtime/src/dirty_ram_interp.c
    ${PSXRECOMP_ROOT}/runtime/src/game_dispatch_compat.c
    ${PSXRECOMP_ROOT}/runtime/src/fntrace.c
    ${PSXRECOMP_ROOT}/runtime/src/text_xlate.cpp
    ${PSXRECOMP_ROOT}/runtime/src/parity_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/device_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/boot_state.c
    ${PSXRECOMP_ROOT}/runtime/src/netplay_snap_ring.c
    ${PSXRECOMP_ROOT}/runtime/src/netplay_state_digest.c
    ${PSXRECOMP_ROOT}/runtime/src/netplay_input_hist.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_netplay_rb.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_netplay_sched.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_selfcheck.c
    ${PSXRECOMP_ROOT}/runtime/src/bios_hle.c
    ${PSXRECOMP_ROOT}/runtime/src/bios_hle_plan.c
    ${PSXRECOMP_ROOT}/runtime/src/savestate.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_savestate_menu.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_rewind.c
    ${PSXRECOMP_ROOT}/runtime/src/host_osd.c
    ${PSXRECOMP_ROOT}/runtime/src/host_keymap.c
    ${PSXRECOMP_ROOT}/runtime/src/cosim_state.c
    ${PSXRECOMP_ROOT}/runtime/src/cosim.c
    ${PSXRECOMP_ROOT}/runtime/src/traps.c
    ${PSXRECOMP_ROOT}/runtime/src/crash_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/freeze_dump_policy.c
    ${PSXRECOMP_ROOT}/runtime/src/freeze_heartbeat.c
    ${PSXRECOMP_ROOT}/runtime/src/gte.cpp
    ${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp
    ${PSXRECOMP_ROOT}/runtime/src/nd_intro_ot.c
    ${PSXRECOMP_ROOT}/runtime/src/crc32.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_sha256.c
    ${PSXRECOMP_ROOT}/runtime/src/disc_identity.cpp
    ${PSXRECOMP_ROOT}/runtime/src/cue_sheet.cpp
    ${PSXRECOMP_ROOT}/runtime/src/disc_path.cpp
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
    ${PSXRECOMP_ROOT}/runtime/src/data_shards.c
    ${PSXRECOMP_ROOT}/runtime/src/load_accel.c
    ${PSXRECOMP_ROOT}/runtime/src/card_read_summary.c
    ${PSXRECOMP_ROOT}/runtime/src/card_data_writes.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_capture.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_loader.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_path_canon.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_posix.c
    ${PSXRECOMP_ROOT}/runtime/src/overlay_backend.c
    ${PSXRECOMP_ROOT}/runtime/src/autocompile.c
    ${PSXRECOMP_ROOT}/runtime/src/code_provider.c
    ${PSXRECOMP_ROOT}/runtime/src/event_ring.c
    ${PSXRECOMP_ROOT}/runtime/src/game_options.c
    ${PSXRECOMP_ROOT}/runtime/src/mod_builtin_speed.c
    ${PSXRECOMP_ROOT}/runtime/src/mod_builtin_pgxp.c
    ${PSXRECOMP_ROOT}/runtime/src/mod_builtin_bezel.c
    ${PSXRECOMP_ROOT}/runtime/src/mod_packages.cpp
    ${PSXRECOMP_ROOT}/runtime/src/mod_runtime.cpp
    ${PSXRECOMP_ROOT}/runtime/src/psx_keybinds.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_bios_backend.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_netplay.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_lobby_client.c
    ${PSXRECOMP_ROOT}/recompiler/src/config_loader.cpp
    ${PSXRECOMP_ROOT}/recompiler/src/ps1_exe_parser.cpp
    # (sljit Tier-2 in-process JIT backend removed 2026-07-15 — was disabled by
    # default since 2026-06-25; gaps fall to the interpreter, gcc/tcc unaffected.)
)

# Optional delay-sync netplay (recomp-net). Auto-discovers a sibling checkout
# (…/recomp-net next to the game repo or next to psxrecomp). Override with
# -DRECOMP_NET_ROOT=… or -DPSX_NETPLAY=OFF.
# OFF by default: netplay is a per-title opt-in, not something every build
# carries. Most titles here are single-player, and an ON default silently
# pulled in the recomp-net library, the lobby WebSocket client and their link
# deps for games that can never use them. A multiplayer title opts in with
# -DPSX_NETPLAY=ON (or sets it before including this file).
option(PSX_NETPLAY "Link recomp-net delay-sync (opt-in; needs recomp-net)" OFF)
# First-run setup wizard + Generate & rebuild (recomp-ui). OFF by default so
# titles that have not tested the self-build flow do not advertise it. Opt in
# with -DPSX_SETUP_WIZARD=ON (or ENABLE_SETUP_WIZARD on psxrecomp_add_game_runtime
# after setting the cache before include, same pattern as PSX_NETPLAY).
option(PSX_SETUP_WIZARD
    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" OFF)
set(RECOMP_NET_ROOT "" CACHE PATH "Path to recomp-net; empty = auto-discover")
if(PSX_NETPLAY AND NOT RECOMP_NET_ROOT)
    foreach(_cand
            "${PSXRECOMP_ROOT}/lib/recomp-net"
            "${CMAKE_SOURCE_DIR}/../recomp-net"
            "${PSXRECOMP_ROOT}/../recomp-net"
            "${CMAKE_SOURCE_DIR}/recomp-net")
        get_filename_component(_abs "${_cand}" ABSOLUTE)
        if(EXISTS "${_abs}/CMakeLists.txt")
            set(RECOMP_NET_ROOT "${_abs}" CACHE PATH "Path to recomp-net; empty = auto-discover" FORCE)
            break()
        endif()
    endforeach()
endif()
if(PSX_NETPLAY AND RECOMP_NET_ROOT AND EXISTS "${RECOMP_NET_ROOT}/CMakeLists.txt")
    if(NOT TARGET recomp_net)
        set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        # MotK lobby ice_p2p needs libjuice. Default ON with netplay; no FORCE
        # so -DRNET_ENABLE_ICE=OFF still wins (LAN-only / offline configure).
        # recomp-net prefers URL FetchContent / third_party/libjuice over git
        # clone (AppImage LD_LIBRARY_PATH breaks system git-remote-https).
        set(RNET_ENABLE_ICE ON CACHE BOOL
            "Build libjuice ICE transport (default ON with PSX_NETPLAY)")
        add_subdirectory("${RECOMP_NET_ROOT}" "${CMAKE_BINARY_DIR}/recomp-net")
    endif()
    set(PSXRECOMP_HAS_RECOMP_NET TRUE)
    if(RNET_ENABLE_ICE)
        message(STATUS "psxrecomp: recomp-net netplay+ICE enabled (${RECOMP_NET_ROOT})")
    else()
        message(STATUS "psxrecomp: recomp-net netplay enabled, ICE off (${RECOMP_NET_ROOT})")
    endif()
else()
    set(PSXRECOMP_HAS_RECOMP_NET FALSE)
    if(PSX_NETPLAY)
        message(STATUS "psxrecomp: recomp-net not found — netplay stubs only "
                       "(set RECOMP_NET_ROOT or place checkout at ../recomp-net)")
    else()
        message(STATUS "psxrecomp: PSX_NETPLAY=OFF — netplay TUs compile as stubs "
                       "(no recomp-net)")
    endif()
endif()

# Portable rollback host policy (sched/hist/hash_confirm/snap). Sits next to
# recomp-net; MotK keeps thin PSX glue (pad↔frame, boot_state, FMV/dig0 gates).
# Snap ring is also used for local rewind without linking full rbengine/recomp-net.
set(RECOMP_RBENGINE_ROOT "" CACHE PATH "Path to retcomm-rbengine; empty = auto-discover")
if(NOT RECOMP_RBENGINE_ROOT)
    foreach(_cand
            "${PSXRECOMP_ROOT}/lib/retcomm-rbengine"
            "${CMAKE_SOURCE_DIR}/../retcomm-rbengine"
            "${PSXRECOMP_ROOT}/../retcomm-rbengine")
        get_filename_component(_abs "${_cand}" ABSOLUTE)
        if(EXISTS "${_abs}/include/retcomm_rbengine/snap_ring.h")
            set(RECOMP_RBENGINE_ROOT "${_abs}" CACHE PATH
                "Path to retcomm-rbengine; empty = auto-discover" FORCE)
            break()
        endif()
    endforeach()
endif()
if(PSXRECOMP_HAS_RECOMP_NET AND RECOMP_RBENGINE_ROOT
   AND EXISTS "${RECOMP_RBENGINE_ROOT}/CMakeLists.txt")
    if(NOT TARGET retcomm_rbengine)
        set(RBE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        # recomp_net already added above; rbengine skips nested add_subdirectory.
        set(RECOMP_NET_ROOT "${RECOMP_NET_ROOT}" CACHE PATH "" FORCE)
        add_subdirectory("${RECOMP_RBENGINE_ROOT}"
                         "${CMAKE_BINARY_DIR}/retcomm-rbengine")
    endif()
    set(PSXRECOMP_HAS_RBENGINE TRUE)
    message(STATUS "psxrecomp: retcomm-rbengine enabled (${RECOMP_RBENGINE_ROOT})")
else()
    set(PSXRECOMP_HAS_RBENGINE FALSE)
    if(PSXRECOMP_HAS_RECOMP_NET)
        message(FATAL_ERROR
            "psxrecomp: PSX_NETPLAY needs retcomm-rbengine.\n"
            "  git submodule update --init lib/retcomm-rbengine\n"
            "  or -DRECOMP_RBENGINE_ROOT=/path/to/retcomm-rbengine")
    endif()
endif()

# Local rewind: full rbengine when netplay is on; otherwise compile snap_ring.c
# only (no recomp-net / sched / hash_confirm). Never both — duplicate symbols.
# Defaults ON because local rewind is useful for every PSX title. If a title
# deliberately opts out with -DPSX_REWIND=OFF, recomp-ui hides the Rewind
# controls instead of exposing dead hotkeys.
option(PSX_REWIND "Build and expose local rewind support" ON)
set(PSXRECOMP_HAS_RBENGINE_SNAP FALSE)
set(PSXRECOMP_RBENGINE_SNAP_INCLUDE "")
if(PSX_REWIND)
    if(PSXRECOMP_HAS_RBENGINE)
        set(PSXRECOMP_HAS_RBENGINE_SNAP TRUE)
    elseif(RECOMP_RBENGINE_ROOT
           AND EXISTS "${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c"
           AND EXISTS "${RECOMP_RBENGINE_ROOT}/include/retcomm_rbengine/snap_ring.h")
        list(APPEND PSXRECOMP_RUNTIME_SOURCES
            ${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c)
        set(PSXRECOMP_HAS_RBENGINE_SNAP TRUE)
        set(PSXRECOMP_RBENGINE_SNAP_INCLUDE "${RECOMP_RBENGINE_ROOT}/include")
        message(STATUS "psxrecomp: rewind snap_ring (${RECOMP_RBENGINE_ROOT})")
    endif()
    if(NOT PSXRECOMP_HAS_RBENGINE_SNAP)
        message(FATAL_ERROR
            "psxrecomp: PSX_REWIND=ON exposes the Rewind launcher controls "
            "but no retcomm-rbengine snap-ring backend was found.\n"
            "  git submodule update --init lib/retcomm-rbengine\n"
            "  or -DRECOMP_RBENGINE_ROOT=/path/to/retcomm-rbengine\n"
            "  or configure with -DPSX_REWIND=OFF to hide Rewind.")
    endif()
else()
    message(STATUS "psxrecomp: local rewind disabled (PSX_REWIND=OFF)")
endif()

# Lobby WebSocket client helpers are vendored under runtime/src/lobby_ws/
# (protocol talks to the proprietary recomp-net-server, not recomp-net).
set(PSXRECOMP_LOBBY_WS_DIR "${PSXRECOMP_ROOT}/runtime/src/lobby_ws")
# Gated on PSX_NETPLAY: this is the client for the netplay lobby server, so it
# is dead weight in a single-player build. It used to enable itself on nothing
# more than the source files existing, i.e. always.
if(PSX_NETPLAY
   AND EXISTS "${PSXRECOMP_LOBBY_WS_DIR}/rnet_ws.c"
   AND EXISTS "${PSXRECOMP_LOBBY_WS_DIR}/rnet_sha1.c")
    set(PSXRECOMP_HAS_LOBBY_CLIENT TRUE)
    list(APPEND PSXRECOMP_RUNTIME_SOURCES
        ${PSXRECOMP_LOBBY_WS_DIR}/rnet_ws.c
        ${PSXRECOMP_LOBBY_WS_DIR}/rnet_sha1.c)
    set(PSXRECOMP_LOBBY_INCLUDE_DIR "${PSXRECOMP_LOBBY_WS_DIR}")
    message(STATUS "psxrecomp: lobby client enabled (${PSXRECOMP_LOBBY_WS_DIR})")
else()
    set(PSXRECOMP_HAS_LOBBY_CLIENT FALSE)
    set(PSXRECOMP_LOBBY_INCLUDE_DIR "")
endif()

set(PSXRECOMP_RUNTIME_INCLUDE_DIRS
    ${PSXRECOMP_ROOT}/runtime/include
    ${PSXRECOMP_ROOT}/recompiler/src
    ${PSXRECOMP_ROOT}/recompiler/include
    ${PSXRECOMP_ROOT}/recompiler/lib/fmt/include
    ${PSXRECOMP_ROOT}/recompiler/lib/toml11
)
if(PSXRECOMP_LOBBY_INCLUDE_DIR)
    list(APPEND PSXRECOMP_RUNTIME_INCLUDE_DIRS ${PSXRECOMP_LOBBY_INCLUDE_DIR})
endif()
if(PSXRECOMP_RBENGINE_SNAP_INCLUDE)
    list(APPEND PSXRECOMP_RUNTIME_INCLUDE_DIRS ${PSXRECOMP_RBENGINE_SNAP_INCLUDE})
endif()

# Which recompiled BIOSes the runtime links. A build carries every image it
# ships — bundled OpenBIOS plus a retail one — and chooses between them at
# startup (docs/BIOS_SELECTION.md). Each image exports a single
# <STEM>_psx_bios_backend descriptor and namespaces everything else, so they
# co-link; the runtime routes psx_dispatch()/psx_bios_image through whichever
# backend it selects.
#
# One build configuration on purpose: the previous per-BIOS flavour needed the
# same choice restated in game.toml and two CMake variables with nothing
# cross-checking them, and it linked cleanly when they disagreed.
set(PSXRECOMP_BUNDLED_BIOS_PATH "bios/openbios.bin" CACHE STRING
    "Bundled redistributable BIOS image, relative to the executable")
set(PSXRECOMP_BUNDLED_BIOS_SOURCE "${PSXRECOMP_ROOT}/bios/openbios.bin" CACHE FILEPATH
    "Source image copied into native runtime builds as the bundled BIOS")
set(PSXRECOMP_BUNDLED_BIOS_LICENSE "${PSXRECOMP_ROOT}/bios/OpenBIOS.LICENSE" CACHE FILEPATH
    "License notice copied alongside the bundled BIOS")
set(PSXRECOMP_BIOS_STEMS "OpenBIOS;SCPH1001" CACHE STRING
    "Recompiled BIOS stems to link (first bundled/redistributable one is the default at runtime)")
# The profile is still needed as the staleness-stamp input for the primary stem.
list(GET PSXRECOMP_BIOS_STEMS 0 PSXRECOMP_BIOS_STEM_PRIMARY)
set(PSXRECOMP_BIOS_STEM "${PSXRECOMP_BIOS_STEM_PRIMARY}" CACHE STRING
    "Primary recompiled BIOS stem (staleness stamp; see PSXRECOMP_BIOS_STEMS)")
set(PSXRECOMP_BIOS_PROFILE "${PSXRECOMP_ROOT}/bios/${PSXRECOMP_BIOS_STEM}.toml" CACHE FILEPATH
    "BIOS profile TOML this build regenerates from (staleness stamp input)")

# Link a stem only if its generated sources are actually present.
#
# PSXRECOMP_BIOS_STEMS lists every stem this build WOULD like. SCPH1001 is in
# the default list, but its generated C is a derivative of a copyrighted Sony
# BIOS, so it is gitignored and only exists once a developer regenerates it
# from their own dump. Requiring it unconditionally meant a fresh checkout --
# which legitimately has only the bundled MIT OpenBIOS -- failed every game
# link with "undefined reference to SCPH1001_psx_bios_backend", long after
# configure had succeeded. Filtering here keeps SCPH1001 fully supported for
# anyone who has regenerated it, without making it mandatory for everyone else.
set(PSXRECOMP_BIOS_GENERATED "")
set(_psxrt_registry_externs "")
set(_psxrt_registry_entries "")
set(_psxrt_bios_linked "")
set(_psxrt_bios_skipped "")
foreach(_stem IN LISTS PSXRECOMP_BIOS_STEMS)
    # Presence is not enough: a generated/ tree left over from before the
    # backend-descriptor mechanism has both files but defines no descriptor,
    # which links fine at configure time and then fails at link with an
    # undefined reference. Probe the descriptor itself. It is emitted into
    # <stem>_dispatch.c (~1MB), never the multi-megabyte <stem>_full.c, so
    # this scan stays cheap.
    set(_psxrt_desc "")
    if(EXISTS "${PSXRECOMP_ROOT}/generated/${_stem}_dispatch.c")
        file(STRINGS "${PSXRECOMP_ROOT}/generated/${_stem}_dispatch.c" _psxrt_desc
             REGEX "${_stem}_psx_bios_backend" LIMIT_COUNT 1)
    endif()
    if(EXISTS "${PSXRECOMP_ROOT}/generated/${_stem}_full.c" AND _psxrt_desc)
        list(APPEND PSXRECOMP_BIOS_GENERATED
            ${PSXRECOMP_ROOT}/generated/${_stem}_full.c
            ${PSXRECOMP_ROOT}/generated/${_stem}_dispatch.c)
        string(APPEND _psxrt_registry_externs
            "extern const PsxBiosBackend ${_stem}_psx_bios_backend;
")
        string(APPEND _psxrt_registry_entries
            "    &${_stem}_psx_bios_backend,
")
        list(APPEND _psxrt_bios_linked "${_stem}")
    else()
        list(APPEND _psxrt_bios_skipped "${_stem}")
    endif()
endforeach()

if(_psxrt_bios_skipped)
    message(STATUS
        "BIOS backends skipped (generated C missing or predates the backend "
        "descriptor): ${_psxrt_bios_skipped} -- regenerate with "
        "tools/regen_bios.sh --config bios/<stem>.toml")
endif()
# Setup hosts / CI may ship with zero BIOS backends; first-run Generate &
# rebuild emits OpenBIOS (and optional retail) locally before a full link.
option(PSXRECOMP_ALLOW_NO_BIOS
    "Allow linking the runtime with no recompiled BIOS backends (setup host)"
    OFF)
if(NOT _psxrt_bios_linked)
    if(PSXRECOMP_ALLOW_NO_BIOS)
        message(STATUS
            "BIOS backends linked: (none) — setup host; Generate & rebuild "
            "will emit BIOS C locally")
        set(_psxrt_bios_count 0)
    else()
        message(FATAL_ERROR
            "No recompiled BIOS backend available. Wanted: ${PSXRECOMP_BIOS_STEMS}, "
            "but no matching generated/<stem>_full.c + <stem>_dispatch.c were found "
            "under ${PSXRECOMP_ROOT}/generated.\n"
            "Generate at least one before building the runtime:\n"
            "    bash tools/regen_bios.sh --config bios/OpenBIOS.toml\n"
            "(OpenBIOS is bundled and MIT-licensed, so this needs no BIOS dump.)\n"
            "Or configure a setup host with -DPSXRECOMP_ALLOW_NO_BIOS=ON.")
    endif()
else()
    message(STATUS "BIOS backends linked: ${_psxrt_bios_linked}")
    list(LENGTH _psxrt_bios_linked _psxrt_bios_count)
endif()

# ISO C requires at least one initializer. A setup host can intentionally have
# zero linked BIOS backends, so give the generated array one unused null entry
# while keeping its public count at zero. MSVC 19.44 otherwise crashes with
# C1001 in CloseTypeServerPDB when it compiles an empty initializer list.
if(NOT _psxrt_registry_entries)
    set(_psxrt_registry_entries "    0, /* unused: registry count is zero */
")
endif()

# Registry of the compiled-in backends, in preference order. Generated so the
# stem list stays the single source of truth.
set(_psxrt_registry_c "${CMAKE_BINARY_DIR}/psx_bios_registry.c")
file(WRITE "${_psxrt_registry_c}"
"/* Generated by runtime.cmake from PSXRECOMP_BIOS_STEMS — do not edit. */
"
"#include \"psx_bios_backend.h\"

"
"${_psxrt_registry_externs}"
"
const PsxBiosBackend *const psx_bios_registry[] = {
"
"${_psxrt_registry_entries}"
"};
"
"const uint32_t psx_bios_registry_count = ${_psxrt_bios_count}u;
")
list(APPEND PSXRECOMP_BIOS_GENERATED "${_psxrt_registry_c}")

# --- BIOS generated/ staleness check (hygiene) -----------------------------------
# generated/<stem>_*.c is gitignored build output produced by a SEPARATE build
# (recompiler/ -> psxrecomp-bios). Editing the BIOS emitter without re-running
# tools/regen_bios.sh leaves the runtime linking a stale BIOS that no longer matches
# the emitter (this caused a 4439-vs-4406 drift). regen_bios.sh records an emitter
# fingerprint in generated/<stem>.emitter.sha; recompute it here (same profile
# argument as regen_bios.sh passes) and WARN on a mismatch so the staleness is
# impossible to miss. Non-fatal: a stale-but-consistent
# BIOS still builds; opt out with -DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON.
if(NOT PSXRECOMP_SKIP_BIOS_STALE_CHECK AND _psxrt_bios_linked)
    find_program(_psxrt_bash NAMES bash)
    set(_psxrt_stamp "${PSXRECOMP_ROOT}/generated/${PSXRECOMP_BIOS_STEM}.emitter.sha")
    if(_psxrt_bash AND EXISTS "${PSXRECOMP_ROOT}/tools/bios_emitter_fingerprint.sh")
        execute_process(
            COMMAND "${_psxrt_bash}" "${PSXRECOMP_ROOT}/tools/bios_emitter_fingerprint.sh"
                    "${PSXRECOMP_BIOS_PROFILE}"
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
                    "  Linking generated/${PSXRECOMP_BIOS_STEM}_*.c that may not "
                    "match the current emitter source, seeds, ROM or profile.\n"
                    "  Fix:  tools/regen_bios.sh --config <profile>   (rebuilds "
                    "psxrecomp-bios + regenerates the BIOS)\n"
                    "  (Suppress: -DPSXRECOMP_SKIP_BIOS_STALE_CHECK=ON)")
            endif()
        endif()
    endif()
endif()

# zlib for boot_state v4. System packages first; otherwise FetchContent a pinned
# release so portable Windows cmake-clang-v1 setups can configure without MSYS2.
option(PSX_ZLIB_FETCH
    "Fetch zlib when no system ZLIB package is found"
    ON)
function(psxrecomp_ensure_zlib)
    if(TARGET ZLIB::ZLIB)
        return()
    endif()
    # Prefer pack zlib via ZLIB_ROOT — deps/ (1.0.9+) then legacy pack root.
    # Never use CMAKE_PREFIX_PATH=pack on Windows llvm-mingw (libc++ clash).
    if(NOT ZLIB_ROOT)
        foreach(_psx_tc_pfx IN LISTS _PSX_TOOLCHAIN_PREFIX_HINTS)
            foreach(_psx_z_root IN ITEMS "${_psx_tc_pfx}/deps" "${_psx_tc_pfx}")
                if(EXISTS "${_psx_z_root}/include/zlib.h" AND
                   (EXISTS "${_psx_z_root}/lib/libz.a" OR
                    EXISTS "${_psx_z_root}/lib/zlib.lib"))
                    set(ZLIB_ROOT "${_psx_z_root}")
                    break()
                endif()
            endforeach()
            if(ZLIB_ROOT)
                break()
            endif()
        endforeach()
    endif()
    # PSX_STATIC_RUNTIME promises no non-system DLL imports. MSYS2's
    # ZLIB::ZLIB is usually shared (zlib1.dll); find_package would leave
    # that import in the host even with -static-libgcc.
    set(_psx_zlib_saved_suffixes "")
    if(PSX_STATIC_RUNTIME)
        set(ZLIB_USE_STATIC_LIBS ON)
        if(MINGW)
            set(_psx_zlib_saved_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
            set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        endif()
    endif()
    find_package(ZLIB QUIET)
    if(NOT "${_psx_zlib_saved_suffixes}" STREQUAL "")
        set(CMAKE_FIND_LIBRARY_SUFFIXES "${_psx_zlib_saved_suffixes}")
    endif()
    if(TARGET ZLIB::ZLIB)
        if(PSX_STATIC_RUNTIME)
            message(STATUS "psxrecomp: ZLIB static (PSX_STATIC_RUNTIME)")
        endif()
        return()
    endif()
    if(NOT PSX_ZLIB_FETCH)
        message(FATAL_ERROR
            "ZLIB was not found. On Windows, use a cmake-clang-v1 pack that "
            "ships deps/include/zlib.h + deps/lib/libz.a (sets ZLIB_ROOT), or "
            "install zlib-devel / mingw-w64-zlib, or configure with "
            "-DPSX_ZLIB_FETCH=ON.")
    endif()
    message(STATUS
        "psxrecomp: ZLIB not in ZLIB_ROOT / system paths; "
        "fetching zlib 1.3.1 (prefer a toolchain pack that bundles it)")
    include(FetchContent)
    set(_psx_zlib_timestamp_args "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        list(APPEND _psx_zlib_timestamp_args DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    psxrecomp_dependency_source_dir(psx_zlib
        ENV PSX_ZLIB_SOURCE_DIR
        OUT _psx_zlib_src)
    psxrecomp_dependency_archive(psx_zlib
        SOURCE_DIR "${_psx_zlib_src}"
        OUT_URL _psx_zlib_url OUT_HASH _psx_zlib_hash)
    FetchContent_Declare(psx_zlib
        URL
            "${_psx_zlib_url}"
        URL_HASH
            "${_psx_zlib_hash}"
        ${_psx_zlib_timestamp_args})
    FetchContent_MakeAvailable(psx_zlib)
    # madler/zlib builds zlibstatic even when a shared zlib target exists.
    # Prefer static when PSX_STATIC_RUNTIME so the host does not import zlib1.dll.
    if(PSX_STATIC_RUNTIME AND TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        message(STATUS "psxrecomp: ZLIB via FetchContent (zlibstatic)")
        return()
    endif()
    if(TARGET ZLIB::ZLIB)
        message(STATUS "psxrecomp: ZLIB via FetchContent (ZLIB::ZLIB)")
        return()
    endif()
    if(TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        message(STATUS "psxrecomp: ZLIB via FetchContent (zlibstatic)")
        return()
    endif()
    if(TARGET zlib)
        add_library(ZLIB::ZLIB ALIAS zlib)
        message(STATUS "psxrecomp: ZLIB via FetchContent (zlib)")
        return()
    endif()
    message(FATAL_ERROR
        "Fetched zlib but no linkable target was produced "
        "(expected zlibstatic or zlib).")
endfunction()

# ---------------------------------------------------------------------------
# Mod catalog staging -- THE FRAMEWORK OWNS THE LAYOUT
# ---------------------------------------------------------------------------
# docs/MOD_PACKAGES.md defines <exe-dir>/mods/bundled as "build output -- the
# framework's mods/builtin/packages plus the title's mods/preloaded/packages".
# Until now only the first half of that sentence was implemented here, and the
# second half was implemented five times, once per title, as a hand-written
#
#     add_custom_command(TARGET psx-runtime POST_BUILD
#         COMMAND ${CMAKE_COMMAND} -E copy_directory
#             "${<TITLE>_PRELOADED_MODS}" "$<TARGET_FILE_DIR:psx-runtime>/mods")
#
# in ApeEscapeRecomp, Tomba2Recomp, MegaManX4/5/6Recomp. Because those blocks
# name the destination as a STRING, framework commit 4cc04be3 -- which renamed
# the staged directory from mods/packages to mods/bundled so a rebuild could
# stop deleting the player's installed mods -- broke all five without breaking
# anything a compiler, linker or configure step can see. The titles kept
# writing mods/packages; the launcher and the release packager had moved to
# mods/bundled. Discovery was deferred to whichever title next ran a release
# packager (bead beads-eio.3.101).
#
# So the layout string now appears in exactly one place. A title declares WHERE
# its packages live (PRELOADED_MODS_DIR) and never HOW they are laid out, and a
# future rename touches this file only.
#
# Two guards keep it that way:
#   * configure time -- a title that has mods/preloaded/packages in its source
#     tree but did not declare it is a hard configure error, because that is
#     exactly the missed-title state, and it is detectable before any build.
#   * build time -- runtime/psx_check_mod_catalog.cmake runs as the LAST
#     POST_BUILD step (registered via cmake_language(DEFER), so it lands after
#     anything the title itself registered) and fails the build if a declared
#     package did not reach mods/bundled, or if any package this build stages
#     turned up in the legacy mods/packages instead.

# Immediate subdirectory names of `root` (package ids), sorted.
function(_psxrt_package_ids root out_var)
    set(_ids "")
    file(GLOB _entries CONFIGURE_DEPENDS LIST_DIRECTORIES true "${root}/*")
    foreach(_e IN LISTS _entries)
        if(IS_DIRECTORY "${_e}")
            get_filename_component(_n "${_e}" NAME)
            list(APPEND _ids "${_n}")
        endif()
    endforeach()
    list(SORT _ids)
    set(${out_var} "${_ids}" PARENT_SCOPE)
endfunction()

# Write `content` to `path` only when it differs, so re-configuring does not
# churn the mtime of a file the build depends on.
function(_psxrt_write_if_changed path content)
    set(_prev "")
    if(EXISTS "${path}")
        file(READ "${path}" _prev)
    endif()
    if(NOT _prev STREQUAL "${content}")
        file(WRITE "${path}" "${content}")
    endif()
endfunction()

# Registered via cmake_language(DEFER) so it runs at the END of the directory
# that created the runtime targets. POST_BUILD commands execute in registration
# order, so deferring is what puts this check AFTER any add_custom_command a
# title registered after its psxrecomp_add_runtime_target() call -- including
# the stray legacy copy this check exists to catch.
#
# Takes NO arguments and reads the pending targets out of global properties:
# cmake_language(DEFER CALL <fn> <arg>) does not carry a function-local
# variable through (measured with cmake 4.2.2 -- the callee receives an empty
# argument, and an add_custom_command built from it fails at directory end
# with "CMakeLists.txt:DEFERRED"), so nothing may be passed positionally here.
function(_psxrt_finalize_mod_catalog_guards)
    get_property(_targets   GLOBAL PROPERTY PSXRECOMP_MOD_CATALOG_TARGETS)
    get_property(_manifests GLOBAL PROPERTY PSXRECOMP_MOD_CATALOG_MANIFESTS)
    get_property(_dirs      GLOBAL PROPERTY PSXRECOMP_MOD_CATALOG_DIRS)
    list(LENGTH _targets _n)
    if(_n EQUAL 0)
        return()
    endif()
    math(EXPR _last "${_n} - 1")

    # add_custom_command(TARGET) only accepts a target created in the current
    # directory, and a deferred call runs in the directory that scheduled it,
    # so a project with runtime targets in several directories gets one
    # deferred pass per directory and each pass handles only its own.
    set(_here_targets "")
    set(_here_manifests "")
    foreach(_i RANGE 0 ${_last})
        list(GET _dirs ${_i} _d)
        if(_d STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
            list(GET _targets ${_i} _t)
            list(GET _manifests ${_i} _m)
            list(APPEND _here_targets "${_t}")
            list(APPEND _here_manifests "${_m}")
        endif()
    endforeach()
    list(LENGTH _here_targets _n_here)
    if(_n_here EQUAL 0)
        return()
    endif()
    math(EXPR _last_here "${_n_here} - 1")

    foreach(_i RANGE 0 ${_last_here})
        list(GET _here_targets ${_i} _t)
        list(GET _here_manifests ${_i} _own)

        # Sibling runtime targets in this directory. Two of them can share one
        # output directory (Tomba 2's US and Italian runtimes both land in the
        # build root), in which case mods/bundled holds whichever linked last.
        set(_alts "")
        foreach(_j RANGE 0 ${_last_here})
            if(NOT _j EQUAL _i)
                list(GET _here_manifests ${_j} _m)
                list(APPEND _alts "${_m}")
            endif()
        endforeach()
        # "|" not ";": a semicolon inside an add_custom_command argument is a
        # cmake list separator and would split the -D into two arguments.
        list(JOIN _alts "|" _alts_joined)

        add_custom_command(TARGET ${_t} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DPSX_MODS_DIR=$<TARGET_FILE_DIR:${_t}>/mods"
                "-DPSX_CATALOG_MANIFEST=${_own}"
                "-DPSX_CATALOG_ALT_MANIFESTS=${_alts_joined}"
                "-DPSX_REQUIRE_STAGED=1"
                "-DPSX_LABEL=${_t}"
                -P "${PSXRECOMP_ROOT}/runtime/psx_check_mod_catalog.cmake"
            COMMENT "Verifying staged mod catalog for ${_t}"
            VERBATIM)
    endforeach()

    # One ctest, registered against the first staging target's output
    # directory. REQUIRE_STAGED is 0 here: `ctest` may run in a tree where the
    # runtime was never built, and a skip is more useful there than a spurious
    # failure. The POST_BUILD invocations above pass 1, since they run
    # immediately after staging where an absent catalog IS the defect.
    get_property(_test_done GLOBAL PROPERTY PSXRECOMP_MOD_CATALOG_TEST_ADDED)
    if(BUILD_TESTING AND NOT _test_done)
        set_property(GLOBAL PROPERTY PSXRECOMP_MOD_CATALOG_TEST_ADDED TRUE)
        list(GET _here_targets 0 _first)
        list(GET _here_manifests 0 _first_manifest)
        set(_first_alts "")
        foreach(_j RANGE 0 ${_last_here})
            if(NOT _j EQUAL 0)
                list(GET _here_manifests ${_j} _m)
                list(APPEND _first_alts "${_m}")
            endif()
        endforeach()
        list(JOIN _first_alts "|" _first_alts_joined)
        add_test(NAME psx_staged_mod_catalog_test
            COMMAND ${CMAKE_COMMAND}
                "-DPSX_MODS_DIR=$<TARGET_FILE_DIR:${_first}>/mods"
                "-DPSX_CATALOG_MANIFEST=${_first_manifest}"
                "-DPSX_CATALOG_ALT_MANIFESTS=${_first_alts_joined}"
                "-DPSX_REQUIRE_STAGED=0"
                "-DPSX_LABEL=${_first}"
                -P "${PSXRECOMP_ROOT}/runtime/psx_check_mod_catalog.cmake")
    endif()
endfunction()

# Stage the framework's builtin packages and the title's own packages into
# <exe-dir>/mods/bundled, and register the guards described above.
function(_psxrt_stage_mod_catalog target preloaded_dir)
    set(_out "$<TARGET_FILE_DIR:${target}>")
    set(_ids "")
    set(_copy "")

    # ---- framework-owned builtins (psx.*) ---------------------------------
    # These target game_id "*" -- emulated-hardware features rather than
    # per-disc content -- so every game gets them without carrying a copy of
    # the manifests.
    set(_builtin_root "${PSXRECOMP_ROOT}/mods/builtin/packages")
    if(EXISTS "${_builtin_root}")
        if(DEFINED PSX_BUILTIN_MOD_ALLOWLIST AND NOT
           "${PSX_BUILTIN_MOD_ALLOWLIST}" STREQUAL "")
            set(_builtin_ids ${PSX_BUILTIN_MOD_ALLOWLIST})
            foreach(_id IN LISTS _builtin_ids)
                if(NOT EXISTS "${_builtin_root}/${_id}")
                    message(FATAL_ERROR
                        "PSX_BUILTIN_MOD_ALLOWLIST names missing package: ${_id}")
                endif()
            endforeach()
        else()
            _psxrt_package_ids("${_builtin_root}" _builtin_ids)
        endif()
        foreach(_id IN LISTS _builtin_ids)
            list(APPEND _ids "${_id}")
            list(APPEND _copy
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_builtin_root}/${_id}"
                    "${_out}/mods/bundled/${_id}")
        endforeach()
    endif()

    # ---- title-owned packages ---------------------------------------------
    set(_readme_copy "")
    if(preloaded_dir STREQUAL "")
        # The missed-title tripwire. A title whose source tree carries a mod
        # catalog but does not declare it used to stage it itself into the
        # wrong directory and find out at release time; now it cannot configure.
        #
        # Requires at least one package: the project scaffold creates an EMPTY
        # mods/preloaded/packages (with a .gitkeep), and a title that has not
        # written a mod yet must still configure.
        set(_undeclared_ids "")
        if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/mods/preloaded/packages")
            _psxrt_package_ids(
                "${CMAKE_SOURCE_DIR}/mods/preloaded/packages" _undeclared_ids)
        endif()
        if(_undeclared_ids)
            list(JOIN _undeclared_ids "\n    " _undeclared_pretty)
            message(FATAL_ERROR
                "This project has a mod catalog at "
                "${CMAKE_SOURCE_DIR}/mods/preloaded/packages:\n"
                "    ${_undeclared_pretty}\n"
                "but target '${target}' did not declare it, so those packages "
                "would not be staged into <exe-dir>/mods/bundled and would not "
                "appear on the Mods page or in a release.\n\n"
                "Add it to the psxrecomp_add_runtime_target() call:\n\n"
                "    psxrecomp_add_runtime_target(${target}\n"
                "        ...\n"
                "        PRELOADED_MODS_DIR \"\${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded\"\n"
                "    )\n\n"
                "and DELETE any add_custom_command(TARGET ${target} POST_BUILD "
                "... copy_directory ... /mods) block: the framework now stages "
                "both its own mods/builtin/packages and the title's packages, "
                "so the layout lives in one place instead of six repositories. "
                "Pass PRELOADED_MODS_DIR NONE if this target genuinely ships "
                "no game catalog.\n\n"
                "See docs/MOD_PACKAGES.md and bead beads-eio.3.101.")
        endif()
    elseif(NOT preloaded_dir STREQUAL "NONE")
        if(NOT IS_DIRECTORY "${preloaded_dir}")
            message(FATAL_ERROR
                "PRELOADED_MODS_DIR for target '${target}' is not a directory: "
                "${preloaded_dir}")
        endif()
        # An empty (or not-yet-created) packages/ subdirectory is the project
        # scaffold's initial state and must still configure -- only a bad path
        # is an error, because a wrong path is exactly how a title ends up
        # shipping an empty Mods page.
        set(_game_ids "")
        if(IS_DIRECTORY "${preloaded_dir}/packages")
            _psxrt_package_ids("${preloaded_dir}/packages" _game_ids)
        endif()
        if(NOT _game_ids)
            message(STATUS
                "psxrecomp: ${target} declares an empty mod catalog at "
                "${preloaded_dir}/packages; staging framework packages only")
        endif()
        foreach(_id IN LISTS _game_ids)
            list(APPEND _ids "${_id}")
            list(APPEND _copy
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${preloaded_dir}/packages/${_id}"
                    "${_out}/mods/bundled/${_id}")
        endforeach()
        # mods/README.md used to arrive only as a side effect of copying the
        # whole mods/preloaded tree into mods/. Now that only packages/ is
        # staged, it has to be copied on purpose or it silently disappears.
        if(EXISTS "${preloaded_dir}/README.md")
            set(_readme_copy
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${preloaded_dir}/README.md"
                    "${_out}/mods/README.md")
        endif()
    endif()

    # The COPY commands above may legitimately name an id twice -- a title's
    # catalog is allowed to OVERRIDE a framework builtin at the same id and
    # version, and Tomba 2's Italian catalog does exactly that with localized
    # psx.* manifests. Order carries that: the framework's copy runs first and
    # the title's overwrites it. The id LIST, though, is an identity set used
    # for the legacy purge, the staged-catalog assertion and the reported
    # count, so it must be deduplicated or the build claims to stage 11
    # packages when the catalog holds 7.
    list(REMOVE_DUPLICATES _ids)
    if(NOT _ids)
        return()
    endif()

    # Removing the ids THIS build stages from any pre-existing mods/packages
    # migrates a build directory made before the bundled/ split, so the
    # build-time guard's "an id turned up in mods/packages" report can only
    # mean a stray copy made by this build. Ids the build does not stage are
    # deliberately left alone: on a self-compiling setup release they can be
    # packages the player installed under the old layout, and the runtime's
    # migrate_legacy_root() relocates those into mods/installed on next launch.
    set(_purge "")
    foreach(_id IN LISTS _ids)
        list(APPEND _purge "${_out}/mods/packages/${_id}")
    endforeach()

    list(LENGTH _ids _n_ids)
    add_custom_command(TARGET ${target} POST_BUILD
        # Wipe first: copy_directory MERGES, so a package deleted from source
        # would otherwise survive in the build output forever and keep
        # appearing on the Mods page (and inflate release catalog assertions).
        #
        # Scoped to mods/bundled, which is build output and nothing else.
        # mods/installed/ is the launcher's (player-installed .psxmod archives)
        # and mods/state.toml is user selection state; a build must never touch
        # either. Before the split this wipe was scoped to mods/packages, which
        # ALSO held everything the player had installed -- so a rebuild,
        # including the one a self-compiling setup release runs on the player's
        # own machine, deleted their mods without a word.
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_out}/mods/bundled"
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_purge}
        ${_copy}
        ${_readme_copy}
        COMMENT "Staging mod catalog for ${target} (${_n_ids} package(s) -> mods/bundled)"
        VERBATIM)

    # The id list the build-time guard and the ctest assert against.
    set(_manifest "${CMAKE_CURRENT_BINARY_DIR}/psx_mod_catalog_${target}.txt")
    list(SORT _ids)
    string(JOIN "\n" _manifest_text ${_ids})
    _psxrt_write_if_changed("${_manifest}" "${_manifest_text}\n")

    set_property(GLOBAL APPEND PROPERTY PSXRECOMP_MOD_CATALOG_TARGETS "${target}")
    set_property(GLOBAL APPEND PROPERTY PSXRECOMP_MOD_CATALOG_MANIFESTS "${_manifest}")
    set_property(GLOBAL APPEND PROPERTY PSXRECOMP_MOD_CATALOG_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}")
    # Schedule the guard pass once per directory, not once per target.
    get_property(_scheduled DIRECTORY PROPERTY PSXRECOMP_MOD_CATALOG_DEFERRED)
    if(NOT _scheduled)
        set_property(DIRECTORY PROPERTY PSXRECOMP_MOD_CATALOG_DEFERRED TRUE)
        cmake_language(DEFER CALL _psxrt_finalize_mod_catalog_guards)
    endif()
endfunction()

function(psxrecomp_add_runtime_target target)
    # PGXP: build this target's objects with -DPSX_PGXP=1 so the PGXP_*()
    # hook macros the emitter writes into ALL generated C become real calls
    # into the value-propagation engine (pgxp_hooks.h; docs/ENHANCEMENTS.md G1.10),
    # and stamp the pgxp overlay flavor so the shard cache and the ABI gate
    # keep pgxp and base DLLs fully separate. The base target is untouched —
    # the macros preprocess away without the define.
    # PGXP_CLONE is internal: set only by the PSX_PGXP_VARIANT auto-clone at the
    # end of this function, to mark the sibling that needs a distinguishing exe
    # name. Callers pass PGXP alone.
    set(options ORACLE COSIM PGXP PGXP_CLONE)
    set(oneValueArgs
        GAME_GENERATED_DISPATCH_C
        GAME_OVERLAY_STATIC_C
        BIOS_GENERATED_FULL_C
        BIOS_GENERATED_DISPATCH_C
        DEBUG_PORT
        WINDOW_TITLE
        DEFAULT_BIOS_PATH
        DEFAULT_GAME_CONFIG_PATH
        LAUNCHER_BOXART
        LAUNCHER_PAD
        LAUNCHER_BRAND
        EXE_NAME
        GAME_VERSION
        MAX_PLAYERS
        APP_ICON
        # The title's own mod catalog source directory -- the one shaped like
        # <repo>/mods/preloaded, holding packages/<id>/<version>/manifest.toml
        # and an optional README.md. The FRAMEWORK stages it, together with its
        # own mods/builtin/packages, into <exe-dir>/mods/bundled. Titles must
        # not stage it themselves; see _psxrt_stage_mod_catalog below for why
        # that is now a build error rather than a convention. Pass the literal
        # NONE to declare, explicitly, that this target ships no game catalog.
        PRELOADED_MODS_DIR
    )
    # GAME_GENERATED_FULL_C is a list (not a single value): the split-TU build
    # writes the recompiled game as N full_NN.c shards instead of one
    # monolithic full.c, so this argument may carry 1..N paths. A single path
    # is just a one-element list, so games still passing one file are
    # unaffected.
    set(multiValueArgs EXTRAS_SOURCES GAME_GENERATED_FULL_C)
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
    # The baked default BIOS path must never be absolute: an absolute path is a
    # build-machine path, and a promoted/release exe carrying it will silently
    # load the BUILDER'S BIOS wherever that path exists (i.e. on the dev box) —
    # so the "prompts for a BIOS on a clean install" flow is never exercised
    # where releases are validated. Dev checkouts still resolve the relative
    # default without prompting via the exe-dir upward search, which also tries
    # <ancestor>/psxrecomp-v4/<relative> for game-project layouts.
    if(NOT PSXRT_DEFAULT_BIOS_PATH)
        set(PSXRT_DEFAULT_BIOS_PATH "bios/SCPH1001.BIN")
    elseif(IS_ABSOLUTE "${PSXRT_DEFAULT_BIOS_PATH}")
        message(WARNING
            "DEFAULT_BIOS_PATH '${PSXRT_DEFAULT_BIOS_PATH}' is absolute; refusing to "
            "bake a build-machine path into the binary (release exes must prompt on "
            "user machines). Using relative 'bios/SCPH1001.BIN' instead — drop the "
            "DEFAULT_BIOS_PATH argument from this game's CMakeLists.")
        set(PSXRT_DEFAULT_BIOS_PATH "bios/SCPH1001.BIN")
    endif()
    if(NOT DEFINED PSXRT_DEFAULT_GAME_CONFIG_PATH)
        set(PSXRT_DEFAULT_GAME_CONFIG_PATH "")
    endif()

    if(PSXRT_BIOS_GENERATED_FULL_C AND PSXRT_BIOS_GENERATED_DISPATCH_C)
        # Per-game BIOS pin: the pinned files REPLACE the matching stem's
        # fork-global generated files only. Every other backend and the
        # generated psx_bios_registry.c stay linked — replacing the whole set
        # (the previous behaviour) dropped the registry and the other
        # backends, producing undefined references to psx_bios_registry at
        # link time. The registry's extern for the pinned stem is satisfied
        # by the pinned dispatch.c, which carries the same backend descriptor.
        get_filename_component(_psxrt_pin_name "${PSXRT_BIOS_GENERATED_DISPATCH_C}" NAME)
        string(REPLACE "_dispatch.c" "" _psxrt_pin_stem "${_psxrt_pin_name}")
        set(generated_sources "")
        foreach(_psxrt_src IN LISTS PSXRECOMP_BIOS_GENERATED)
            get_filename_component(_psxrt_src_name "${_psxrt_src}" NAME)
            if(_psxrt_src_name STREQUAL "${_psxrt_pin_stem}_full.c" OR
               _psxrt_src_name STREQUAL "${_psxrt_pin_stem}_dispatch.c")
                # replaced by the game's pinned copy
            else()
                list(APPEND generated_sources "${_psxrt_src}")
            endif()
        endforeach()
        list(APPEND generated_sources
            "${PSXRT_BIOS_GENERATED_FULL_C}"
            "${PSXRT_BIOS_GENERATED_DISPATCH_C}")
        set_source_files_properties(${generated_sources} PROPERTIES GENERATED TRUE)
        message(STATUS "psxrecomp: BIOS stem ${_psxrt_pin_stem} pinned to game-local copies")
    else()
        set(generated_sources ${PSXRECOMP_BIOS_GENERATED})
    endif()
    # Game recompiled C that a from-source builder must generate before building
    # (see the require-generated guard added after add_executable below). Collected
    # here so the guard names the exact files that are missing.
    set(_game_generated_check "")
    if(PSXRT_GAME_GENERATED_FULL_C)
        foreach(_full_src IN LISTS PSXRT_GAME_GENERATED_FULL_C)
            set_source_files_properties("${_full_src}" PROPERTIES GENERATED TRUE)
            list(APPEND generated_sources "${_full_src}")
            list(APPEND _game_generated_check "${_full_src}")
        endforeach()
        set(has_game_dispatch TRUE)
    endif()
    if(PSXRT_GAME_GENERATED_DISPATCH_C)
        set_source_files_properties("${PSXRT_GAME_GENERATED_DISPATCH_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_GENERATED_DISPATCH_C}")
        list(APPEND _game_generated_check "${PSXRT_GAME_GENERATED_DISPATCH_C}")
        set(has_game_dispatch TRUE)
        if(EXISTS "${PSXRT_GAME_GENERATED_DISPATCH_C}")
            file(STRINGS "${PSXRT_GAME_GENERATED_DISPATCH_C}"
                game_dispatch_native_ok_decl
                REGEX "int[ \t]+psx_game_text_native_ok\\("
                LIMIT_COUNT 1)
            if(game_dispatch_native_ok_decl)
                set(has_game_dispatch_native_ok TRUE)
            endif()
            file(STRINGS "${PSXRT_GAME_GENERATED_DISPATCH_C}"
                game_dispatch_native_ok_full_decl
                REGEX "int[ \t]+psx_game_text_native_ok_full\\("
                LIMIT_COUNT 1)
            if(game_dispatch_native_ok_full_decl)
                set(has_game_dispatch_native_ok_full TRUE)
            endif()
        endif()
    endif()
    # Layer B: statically-compiled overlay dispatch. Inert unless a game
    # provides a generated overlays_static.c — no target sets this yet.
    if(PSXRT_GAME_OVERLAY_STATIC_C AND EXISTS "${PSXRT_GAME_OVERLAY_STATIC_C}")
        set_source_files_properties("${PSXRT_GAME_OVERLAY_STATIC_C}" PROPERTIES GENERATED TRUE)
        list(APPEND generated_sources "${PSXRT_GAME_OVERLAY_STATIC_C}")
        # compile_overlays.py --static splits its output: overlays_static.c is
        # the dispatcher and each overlay is its own translation unit,
        # overlays_static_NNNN.c, beside it. One 300 MB file compiled on one
        # core is what that replaced. CONFIGURE_DEPENDS re-globs at build time,
        # so a run that changes the part count needs no reconfigure (a
        # --static-single-file run simply has no parts to glob).
        get_filename_component(_ov_static_dir  "${PSXRT_GAME_OVERLAY_STATIC_C}" DIRECTORY)
        get_filename_component(_ov_static_stem "${PSXRT_GAME_OVERLAY_STATIC_C}" NAME_WE)
        file(GLOB _ov_static_parts CONFIGURE_DEPENDS
             "${_ov_static_dir}/${_ov_static_stem}_[0-9][0-9][0-9][0-9].c")
        list(SORT _ov_static_parts)
        foreach(_ov_part IN LISTS _ov_static_parts)
            set_source_files_properties("${_ov_part}" PROPERTIES GENERATED TRUE)
        endforeach()
        list(APPEND generated_sources ${_ov_static_parts})
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
    target_link_libraries(${target} PRIVATE chdr-static)
    # audio_trace.c uses C11 atomics. Make the runtime's actual language
    # requirement explicit instead of relying on a parent project's global
    # CMAKE_C_STANDARD setting. cxx_std_17 likewise — game CMakeLists may omit
    # CMAKE_CXX_STANDARD; mod_packages.cpp must not compile as a pre-17 dialect.
    target_compile_features(${target} PRIVATE c_std_11 cxx_std_17)

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
    if(PSXRT_PGXP)
        # The _pgxp suffix exists only to keep the auto-cloned A/B sibling
        # distinct from the base binary it sits beside (PSX_PGXP_VARIANT); the
        # launcher or the player picks between them. Same debug port as the
        # base build — run one at a time (the A/B protocol is
        # one-toggle-per-run anyway).
        #
        # A title that builds PGXP into its ONE runtime target has no base
        # binary beside it, so suffixing there would ship a single product
        # under an odd name and, worse, leave a second look-alike executable in
        # the build tree for someone to launch by mistake. That is not
        # hypothetical: a non-PGXP binary got played and reported as "textures
        # still wobble" while the PGXP one measured 99% precise-vertex
        # coverage. Suffix the clone, never the primary.
        if(PSXRT_PGXP_CLONE)
            set(_psxrt_exe_name "${_psxrt_exe_name}_pgxp")
        endif()
        target_compile_definitions(${target} PRIVATE
            PSX_PGXP=1
            PSX_OVERLAY_FLAVOR=2)   # PSX_OVERLAY_FLAVOR_PGXP (overlay_api.h)
        set(_psxrt_overlay_flavor 2)
    else()
        set(_psxrt_overlay_flavor 0)
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${_psxrt_exe_name}")

    # Publish the OVERLAY CODEGEN FLAVOR this target links with, for the same
    # reason the exe name is published just below: so nothing downstream has to
    # re-derive it.
    #
    # The flavor is the high half of overlay_abi() and the `_f<n>` field of the
    # shard cache tag (overlay_api.h PSX_OVERLAY_FLAVOR; 0 base, 2 pgxp). It is
    # a property of the BINARY, decided right here, and it is NOT
    # platform-dependent — a Windows and a Linux build of the same target share
    # it. Every release packager needs it to name the cache namespace the
    # shipped runtime will actually read, and until now every packager simply
    # assumed 0. That assumption is invisible when it is wrong: a PGXP runtime
    # reads cache/<id>/gcc/<arch-abi>/cg..._f2/ while the packager stages
    # ..._f0/, so the package ships a cache the binary ignores completely and
    # every overlay runs interpreted, with nothing failing anywhere.
    #
    # tools/release_stage.py reads this file (cg-tag --flavor-from-build), so a
    # packager can stop guessing. See bead beads-eio.3.102.
    file(GENERATE
         OUTPUT "${CMAKE_BINARY_DIR}/psxrecomp_overlay_flavor-${target}.txt"
         CONTENT "${_psxrt_overlay_flavor}\n")

    # Publish the name we just chose, so nothing downstream has to re-derive it.
    #
    # Two other places used to compute this independently — psxrecomp_cli.py
    # from --exe-name, and the in-runtime self-compiler from
    # codegen_setup.exe_basename — each re-running the same
    # MAKE_C_IDENTIFIER(WINDOW_TITLE) rule against its own copy of the title.
    # The rules agree; the copies drift. Rename a game and the build links
    # <new>.exe while both consumers look for <old>.exe, which surfaces as the
    # flatly untrue "build succeeded but binary missing" on a build that had no
    # errors at all. Seen in the wild on Revelations: Persona, where CMake
    # emitted Revelations__Persona_Recompiled.exe and the setup host wanted
    # Revelations_Persona__Recompiled.exe — same algorithm, colon in a
    # different place.
    #
    # Generate-time output uses the final target property, so a game that
    # changes OUTPUT_NAME after this helper returns still publishes the name
    # CMake will actually link.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/psxrecomp_exe_name-${target}.txt"
         CONTENT "$<TARGET_FILE_BASE_NAME:${target}>\n")

    # ---- Windows / desktop app icon ---------------------------------------
    # Prefer an explicit APP_ICON, then the game-repo copy under assets/, then
    # the framework default shipped in psxrecomp/assets (RetComM-themed pad).
    if(NOT PSXRT_APP_ICON)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico")
            set(PSXRT_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico")
        elseif(EXISTS "${PSXRECOMP_ROOT}/assets/psxrecomp.ico")
            set(PSXRT_APP_ICON "${PSXRECOMP_ROOT}/assets/psxrecomp.ico")
        endif()
    endif()
    if(PSXRT_APP_ICON AND EXISTS "${PSXRT_APP_ICON}")
        if(WIN32)
            # clang/llvm-mingw CI needs an RC compiler or the .rc is ignored and
            # the PE ships without an embedded icon.
            enable_language(RC)
            if(NOT CMAKE_RC_COMPILER)
                find_program(CMAKE_RC_COMPILER
                    NAMES llvm-rc llvm-windres windres
                    HINTS
                        "$ENV{RETCOMM_TOOLCHAIN}/bin"
                        "$ENV{CMAKE_CLANG_V1}/bin"
                    DOC "Windows resource compiler for APP_ICON .rc")
            endif()
            if(CMAKE_RC_COMPILER)
                string(REPLACE "\\" "/" _psxrt_ico_fwd "${PSXRT_APP_ICON}")
                set(_psxrt_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_app_icon.rc")
                file(WRITE "${_psxrt_rc}" "IDI_ICON1 ICON \"${_psxrt_ico_fwd}\"\n")
                target_sources(${target} PRIVATE "${_psxrt_rc}")
                message(STATUS "psxrecomp ${target}: APP_ICON=${PSXRT_APP_ICON} (RC=${CMAKE_RC_COMPILER})")
            else()
                message(WARNING
                    "psxrecomp ${target}: APP_ICON set but no RC compiler "
                    "(llvm-rc/windres) — PE will have no embedded icon; "
                    "runtime still loads assets/psxrecomp.png via SDL")
            endif()
        else()
            message(STATUS "psxrecomp ${target}: APP_ICON=${PSXRT_APP_ICON} (window icon via PNG)")
        endif()
        # Stage PNG beside the exe when present (AppImage / desktop / SDL icon).
        get_filename_component(_psxrt_ico_dir "${PSXRT_APP_ICON}" DIRECTORY)
        set(_psxrt_png "${_psxrt_ico_dir}/psxrecomp.png")
        if(EXISTS "${_psxrt_png}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/assets"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_psxrt_png}"
                    "$<TARGET_FILE_DIR:${target}>/assets/psxrecomp.png"
                COMMENT "Staging psxrecomp.png app icon"
                VERBATIM)
        endif()
    endif()

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

    # ---- require-generated guard -------------------------------------------
    # The game's recompiled C (generated/<serial>_{full,dispatch}.c) is produced
    # by the recompiler tool in a step BEFORE this build, and its paths are marked
    # GENERATED so `cmake configure` succeeds before that step has run. Without a
    # guard, a builder who skips generation only finds out deep in the build via
    #   cc1: fatal error: .../<serial>_full.c: No such file or directory
    # with no hint that a step was skipped or what produces the file. Catch it
    # first — a WARNING now (early, at configure) and a hard, actionable stop at
    # build start (below) — so the raw compiler error is never the first signal.
    # Only guards GAME sources: the BIOS path is either bundled OpenBIOS (emitted
    # by a custom command here) or has its own staleness check above.
    if(_game_generated_check)
        if(EXISTS "${PSXRECOMP_ROOT}/recompiler/build/psxrecomp-game.exe")
            set(_psxrt_recompiler_hint "${PSXRECOMP_ROOT}/recompiler/build/psxrecomp-game.exe")
        else()
            set(_psxrt_recompiler_hint "${PSXRECOMP_ROOT}/recompiler/build/psxrecomp-game")
        endif()
        set(_psxrt_missing_now "")
        foreach(_g IN LISTS _game_generated_check)
            if(NOT EXISTS "${_g}")
                list(APPEND _psxrt_missing_now "${_g}")
            endif()
        endforeach()
        if(_psxrt_missing_now)
            message(WARNING
                "${target}: recompiled game C is not present yet — the build will "
                "fail until you generate it.\n"
                "  Run the recompiler once:  ${_psxrt_recompiler_hint} --config "
                "${PSXRT_DEFAULT_GAME_CONFIG_PATH}\n"
                "  (build that tool first if needed; see psxrecomp/docs/BUILDING.md). "
                "This is expected on a fresh checkout before the first generation.")
        endif()
        # Pass paths via a list file — large shard counts (hundreds of
        # generated/*_full_*.c) make -DSOURCES=... exceed Windows' ~8191-char
        # CreateProcess limit ("The system cannot execute the specified program").
        set(_psxrt_gen_list
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_generated_sources.txt")
        file(WRITE "${_psxrt_gen_list}" "")
        foreach(_g IN LISTS _game_generated_check)
            file(APPEND "${_psxrt_gen_list}" "${_g}\n")
        endforeach()
        add_custom_target(${target}_require_generated
            COMMAND ${CMAKE_COMMAND}
                    "-DSOURCES_FILE=${_psxrt_gen_list}"
                    "-DTARGET=${target}"
                    "-DGAME_CONFIG=${PSXRT_DEFAULT_GAME_CONFIG_PATH}"
                    "-DRECOMPILER=${_psxrt_recompiler_hint}"
                    "-DDOC=psxrecomp/docs/BUILDING.md  (\"Build and run a game\")"
                    -P "${PSXRECOMP_ROOT}/runtime/check_generated_sources.cmake"
            COMMENT "Verifying recompiled game C exists for ${target}"
            VERBATIM)
        # Target-level dependency: this check runs to completion before ANY of
        # ${target}'s objects compile, so a missing generated source aborts with
        # our message rather than the compiler's.
        add_dependencies(${target} ${target}_require_generated)
    endif()

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
        ${PSX_SDL_INCLUDE_DIRS}
    )
    # pkg-config reports fallback SDL2_LIBRARIES as a bare name
    # (e.g. "SDL2" -> -lSDL2);
    # add its library dirs so the linker finds it outside default paths
    # (e.g. Homebrew's /opt/homebrew/lib on macOS). Empty/harmless on MSVC.
    if(PSX_SDL_LIBRARY_DIRS)
        target_link_directories(${target} PRIVATE ${PSX_SDL_LIBRARY_DIRS})
    endif()
    # For a self-contained MinGW SDL2 fallback build, link SDL2 statically via
    # pkg-config's
    # --static link line (libSDL2.a + the full Windows system-lib chain SDL2
    # needs: winmm, imm32, ole32, oleaut32, version, setupapi, dinput8, ...).
    # Otherwise link the SDL2 import lib (needs SDL2.dll at runtime).
    if(PSX_STATIC_RUNTIME AND PSX_SDL_STATIC_LDFLAGS)
        target_link_libraries(${target} PRIVATE ${PSX_SDL_STATIC_LDFLAGS})
    else()
        target_link_libraries(${target} PRIVATE ${PSX_SDL_LIBRARIES})
    endif()

    # zlib: boot_state v4 savestate compression (RAM/VRAM/SPU blobs).
    # Portable Windows toolchains (cmake-clang-v1) have no system zlib —
    # fetch a pinned release when find_package fails (same pattern as SDL3).
    psxrecomp_ensure_zlib()
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB)

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

    # Release pin for lobby matching (create/join/list). Override via
    # GAME_VERSION arg or -DPSX_GAME_VERSION=...; default "dev".
    if(NOT PSXRT_GAME_VERSION)
        if(DEFINED PSX_GAME_VERSION AND NOT PSX_GAME_VERSION STREQUAL "")
            set(PSXRT_GAME_VERSION "${PSX_GAME_VERSION}")
        else()
            set(PSXRT_GAME_VERSION "dev")
        endif()
    endif()

    # Per-game netplay/local pad ceiling. Default 2 (MotK / dual-shock path).
    # Single-player titles (Tomba, Ape Escape, …) pass MAX_PLAYERS 1 so rewind
    # / rbengine still link without advertising multiplayer. Multitap N-player
    # (Bomberman Party Edition) uses 5; dual SCPH-1070 uses 8. Range matches
    # sio.h (1..8).
    if(NOT PSXRT_MAX_PLAYERS)
        if(DEFINED PSX_MAX_PLAYERS AND NOT PSX_MAX_PLAYERS STREQUAL "")
            set(PSXRT_MAX_PLAYERS "${PSX_MAX_PLAYERS}")
        else()
            set(PSXRT_MAX_PLAYERS 2)
        endif()
    endif()
    if(PSXRT_MAX_PLAYERS LESS 1 OR PSXRT_MAX_PLAYERS GREATER 8)
        message(FATAL_ERROR
            "MAX_PLAYERS must be in 1..8 (got ${PSXRT_MAX_PLAYERS})")
    endif()
    message(STATUS "psxrecomp ${target}: PSX_MAX_PLAYERS=${PSXRT_MAX_PLAYERS}")

    target_compile_definitions(${target} PRIVATE
        DEFAULT_DEBUG_PORT=${PSXRT_DEBUG_PORT}
        PSX_DEFAULT_BIOS_PATH="${PSXRT_DEFAULT_BIOS_PATH}"
        # Where the shipped redistributable image lives, relative to the exe.
        # This is what a player gets when they choose no BIOS.
        PSX_BUNDLED_BIOS_PATH="${PSXRECOMP_BUNDLED_BIOS_PATH}"
        PSX_DEFAULT_GAME_CONFIG_PATH="${PSXRT_DEFAULT_GAME_CONFIG_PATH}"
        PSX_WINDOW_TITLE="${PSXRT_WINDOW_TITLE}"
        PSX_MAX_PLAYERS=${PSXRT_MAX_PLAYERS}
        FMT_HEADER_ONLY=1
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        $<$<BOOL:${PSX_SDL3}>:PSX_SDL3=1>
        $<$<CXX_COMPILER_ID:MSVC>:SDL_MAIN_HANDLED>
    )
    # Version / git rev change often on package updates. Keep them off the
    # target-wide compile line so Ninja does not rebuild every runtime + shard TU.
    set_source_files_properties(
        "${PSXRECOMP_ROOT}/runtime/src/psx_lobby_client.c"
        PROPERTIES COMPILE_DEFINITIONS "PSX_GAME_VERSION=\"${PSXRT_GAME_VERSION}\""
    )
    set_source_files_properties(
        "${PSXRECOMP_ROOT}/runtime/src/crash_trace.c"
        PROPERTIES COMPILE_DEFINITIONS "PSX_BUILD_REV=\"${PSX_GIT_REV}\""
    )

    # Stamp the lobby pin next to the exe (and, on multi-config, in the build
    # root). Packagers must ship VERSION == this stamp — rewriting VERSION after
    # the build caused Twisted Metal 4 installs where VERSION said 0.3.8 but the
    # binary still filtered lobbies as 0.3.7.
    #
    # Single-config (Ninja/Make/MinGW): TARGET_FILE_DIR == CMAKE_BINARY_DIR, so a
    # second GENERATE to the same path is rejected ("Files to be generated by
    # multiple different commands"). Multi-config (VS): exe lives in Release/,
    # so also drop a copy at the build root for packager lookup.
    # Once per build tree, not per target: with several runtime targets
    # (psx-runtime + psx-oracle + psx-beetle) each TARGET_FILE_DIR genex is a
    # DIFFERENT command producing the SAME path on single-config generators,
    # which CMake rejects even when the content is identical (the ae9e6a4e
    # hotfix fixed the same-target duplicate but not the multi-target one).
    # All runtime targets in one tree share one PSXRT_GAME_VERSION, so a
    # single stamp is the correct semantics anyway.
    get_property(_psxrt_ver_stamped GLOBAL PROPERTY PSXRT_GAME_VERSION_STAMPED)
    if(NOT _psxrt_ver_stamped)
        set_property(GLOBAL PROPERTY PSXRT_GAME_VERSION_STAMPED 1)
        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:${target}>/psx_game_version.txt"
            CONTENT "${PSXRT_GAME_VERSION}\n"
        )
        get_property(_psxrt_ver_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(_psxrt_ver_multi)
            file(GENERATE
                OUTPUT "${CMAKE_BINARY_DIR}/psx_game_version.txt"
                CONTENT "${PSXRT_GAME_VERSION}\n"
            )
        endif()
    endif()

    # OpenBIOS is part of the native runtime product, not a developer-machine
    # prerequisite. Stage both the exact ROM consumed by the compiled backend
    # and its required MIT notice beside every native executable. Release
    # packagers copy this directory as a unit.
    if(NOT PSXRT_ORACLE)
        list(FIND PSXRECOMP_BIOS_STEMS "OpenBIOS" _psxrt_openbios_index)
        if(NOT _psxrt_openbios_index EQUAL -1)
            if(NOT EXISTS "${PSXRECOMP_BUNDLED_BIOS_SOURCE}")
                message(FATAL_ERROR
                    "Bundled OpenBIOS image is missing: "
                    "${PSXRECOMP_BUNDLED_BIOS_SOURCE}")
            endif()
            if(NOT EXISTS "${PSXRECOMP_BUNDLED_BIOS_LICENSE}")
                message(FATAL_ERROR
                    "Bundled OpenBIOS license is missing: "
                    "${PSXRECOMP_BUNDLED_BIOS_LICENSE}")
            endif()
            get_filename_component(
                _psxrt_bundled_bios_dir
                "${PSXRECOMP_BUNDLED_BIOS_PATH}"
                DIRECTORY)
            get_filename_component(
                _psxrt_bundled_bios_license_name
                "${PSXRECOMP_BUNDLED_BIOS_LICENSE}"
                NAME)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/${_psxrt_bundled_bios_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PSXRECOMP_BUNDLED_BIOS_SOURCE}"
                    "$<TARGET_FILE_DIR:${target}>/${PSXRECOMP_BUNDLED_BIOS_PATH}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PSXRECOMP_BUNDLED_BIOS_LICENSE}"
                    "$<TARGET_FILE_DIR:${target}>/${_psxrt_bundled_bios_dir}/${_psxrt_bundled_bios_license_name}"
                COMMENT "Staging bundled OpenBIOS image and MIT notice"
                VERBATIM)
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
                "${PSXRECOMP_BUNDLED_BIOS_SOURCE}"
                "${PSXRECOMP_BUNDLED_BIOS_LICENSE}")
        endif()

    # Mod catalog: the framework's builtin packages AND the title's own, both
    # staged into mods/bundled by _psxrt_stage_mod_catalog (see its header).
    # Skipped for cosim oracles: they have no launcher and no Mods page, and
    # because a cosim exe lands in the same output directory as the runtime,
    # letting it stage would have it wipe and re-stage the runtime's catalog
    # with only the framework half.
    if(NOT PSXRT_COSIM)
        _psxrt_stage_mod_catalog("${target}" "${PSXRT_PRELOADED_MODS_DIR}")
    endif()
    endif()

    if(PSXRT_ORACLE)
        target_compile_definitions(${target} PRIVATE PSX_ORACLE_BUILD=1)
    else()
        target_compile_definitions(${target} PRIVATE
            PSX_NATIVE_BUILD=1
            PSX_ENABLE_BLOCK_CYCLES=1
        )
    endif()
    if(PSX_SHELLWIN_INTERP)
        target_compile_definitions(${target} PRIVATE PSX_SHELLWIN_INTERP_DEFAULT=1)
    endif()

    # Developer-channel mod features do not ship. A contributor reaches them by
    # cloning the repo and building locally; a release build must not carry
    # them at all -- not hidden behind a toggle, absent. Keying the default off
    # $CI matches what the packagers already do for EXCLUDE_DEV_MODS, so one
    # rule covers the catalog on disk and the catalog in the binary.
    if(NOT DEFINED PSX_MOD_DEVELOPER_CHANNEL)
        if(DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
            set(PSX_MOD_DEVELOPER_CHANNEL OFF)
        else()
            set(PSX_MOD_DEVELOPER_CHANNEL ON)
        endif()
    endif()
    if(PSX_MOD_DEVELOPER_CHANNEL)
        target_compile_definitions(${target} PRIVATE PSX_MOD_DEVELOPER_CHANNEL=1)
    endif()
    if(has_game_dispatch)
        target_compile_definitions(${target} PRIVATE PSX_HAS_GAME_DISPATCH=1)
    endif()
    if(has_game_dispatch_native_ok)
        # Only the compatibility translation unit needs to know whether the
        # generated dispatcher supplies the modern exact-range predicate.
        # Keeping this off the target-wide definitions avoids recompiling the
        # entire runtime when an existing game regenerates its dispatcher.
        set_property(SOURCE
            ${PSXRECOMP_ROOT}/runtime/src/game_dispatch_compat.c
            APPEND PROPERTY COMPILE_DEFINITIONS
            PSX_GAME_DISPATCH_HAS_NATIVE_OK=1)
    endif()
    if(has_game_dispatch_native_ok_full)
        set_property(SOURCE
            ${PSXRECOMP_ROOT}/runtime/src/game_dispatch_compat.c
            APPEND PROPERTY COMPILE_DEFINITIONS
            PSX_GAME_DISPATCH_HAS_NATIVE_OK_FULL=1)
    endif()
    if(has_overlay_dispatch)
        target_compile_definitions(${target} PRIVATE PSX_HAS_OVERLAY_DISPATCH=1)
    endif()

    # PSX_DEBUG_TOOLS option declared at the top of runtime.cmake so it's
    # also visible to psx-beetle / non-runtime-helper targets.
    if(NOT PSX_DEBUG_TOOLS)
        target_compile_definitions(${target} PRIVATE PSX_NO_DEBUG_TOOLS=1)
    endif()

    if(PSXRECOMP_HAS_RECOMP_NET)
        target_compile_definitions(${target} PRIVATE PSX_HAS_RECOMP_NET=1)
        target_link_libraries(${target} PRIVATE recomp_net)
        if(PSXRECOMP_HAS_RBENGINE)
            target_link_libraries(${target} PRIVATE retcomm_rbengine)
        endif()
    endif()
    if(PSXRECOMP_HAS_RBENGINE_SNAP)
        target_compile_definitions(${target} PRIVATE PSX_HAS_RBENGINE_SNAP=1)
    endif()
    if(PSXRECOMP_HAS_LOBBY_CLIENT)
        target_compile_definitions(${target} PRIVATE PSX_HAS_LOBBY_CLIENT=1)
    endif()
    if(PSX_SETUP_WIZARD)
        target_compile_definitions(${target} PRIVATE PSX_HAS_SETUP_WIZARD=1)
    endif()

    # First-divergence co-sim oracle (COSIM_ORACLE.md): the clean, deterministic build.
    # PSX_COSIM activates the cosim engine/hooks; PSX_NO_DEBUG_TOOLS strips ALL the laggy
    # diagnostic tooling (the debug server thread, per-block recording, rings) so the run
    # is single-threaded + fast + deterministic. The two instances (this + a FORCE_INTERP
    # run) are driven in cycle-lockstep by tools/cosim.py.
    if(PSXRT_COSIM)
        target_compile_definitions(${target} PRIVATE PSX_COSIM=1 PSX_NO_DEBUG_TOOLS=1)
    endif()

    # Shared recomp-ui Dear ImGui launcher (not in the oracle build — that's headless).
    # Lives at the game repo root (RECOMP_UI_ROOT / CMAKE_SOURCE_DIR/recomp-ui),
    # not under psxrecomp/lib/.
    if(PSX_RECOMP_UI AND NOT PSXRT_ORACLE)
        if(NOT RECOMP_UI_ROOT OR NOT EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
            message(FATAL_ERROR
                "PSX_RECOMP_UI=ON but recomp-ui is missing.\n"
                "Add at the game repo root:\n"
                "  git submodule add -b master "
                "https://github.com/mstan/recomp-ui.git recomp-ui\n"
                "Or set -DRECOMP_UI_ROOT=/path/to/recomp-ui")
        endif()
        # recomp-ui gates its Mods view behind RECOMP_UI_ENABLE_MODS, which
        # defaults OFF there -- correct for a cross-console launcher, since a
        # console with no mod system should not show an empty Mods tab.
        #
        # psxrecomp does ship mod packages as a first-class, documented feature
        # (docs/MOD_PACKAGES.md; Tomba! and Ape Escape ship catalogs today), so
        # the framework opts in on every title's behalf. Without this, a title
        # that already shipped a Mods panel silently loses it on its next
        # rebuild -- the panel compiles in but stays inert, which reads as "this
        # build is old" rather than as a missing build flag.
        #
        # Set before the include so recomp-ui's option() honours it, and only
        # when the caller has not already decided, so -DRECOMP_UI_ENABLE_MODS=OFF
        # still wins.
        if(NOT DEFINED RECOMP_UI_ENABLE_MODS)
            set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL
                "Enable the recomp-ui Mods view (psxrecomp ships mod packages)")
        endif()
        # The seed above only fires on a FRESH cache. A build tree configured
        # before it existed already has RECOMP_UI_ENABLE_MODS=OFF in its cache,
        # written by recomp-ui's own option(), and nothing can distinguish that
        # stale default from a deliberate -DRECOMP_UI_ENABLE_MODS=OFF. Such a
        # tree therefore keeps producing a Mods-less build across reconfigures,
        # which is the same "reads as a stale build" failure the seed was added
        # to prevent -- just one level up, and invisible.
        #
        # The explicit-OFF contract above is deliberate, so this does not
        # override it. It makes the state audible instead: whoever sees a
        # Mods-less build now gets told why and how to change it, rather than
        # having to query the running game to discover the panel was compiled
        # out. Deliberate opt-outs get one line per configure, which is the
        # price of the two cases being genuinely indistinguishable.
        if(NOT RECOMP_UI_ENABLE_MODS)
            message(WARNING
                "RECOMP_UI_ENABLE_MODS is OFF in this build tree, so the "
                "launcher will have NO Mods page even for a title that ships a "
                "catalog.\n"
                "  If that was not deliberate, this cache predates the "
                "framework opting in: delete CMakeCache.txt (or just that "
                "entry) to pick up the ON default.")
        endif()
        set(RECOMP_UI_SDL3 ${PSX_SDL3})
        include("${RECOMP_UI_ROOT}/recomp_ui.cmake")

        # Asset staging is console-scoped in recomp-ui. Select PSX once here so
        # every game using this framework ships only PlayStation launcher art
        # (plus common chrome), never unrelated NES/N64/etc. assets.
        set(_psx_recomp_ui_args CONSOLE psx)
        if(PSXRT_LAUNCHER_BOXART)
            list(APPEND _psx_recomp_ui_args BOXART "${PSXRT_LAUNCHER_BOXART}")
        endif()
        if(PSXRT_LAUNCHER_PAD)
            list(APPEND _psx_recomp_ui_args PAD "${PSXRT_LAUNCHER_PAD}")
        endif()
        if(PSXRT_LAUNCHER_BRAND)
            list(APPEND _psx_recomp_ui_args BRAND "${PSXRT_LAUNCHER_BRAND}")
        endif()
        recomp_target_launcher_ui(${target} ${_psx_recomp_ui_args})
        target_compile_definitions(${target} PRIVATE
            RECOMP_UI_PSX_HAS_REWIND=$<BOOL:${PSXRECOMP_HAS_RBENGINE_SNAP}>)
    endif()

    if(WIN32 OR MINGW)
        # opengl32: GL backend (gpu_gl_renderer.c). GL 1.x is exported directly
        # by opengl32; Phase 2b will load modern GL via SDL_GL_GetProcAddress.
        # iphlpapi: GetAdaptersAddresses in the launcher's netplay
        # local-address discovery (MSVC does not link it implicitly).
        target_link_libraries(${target} PRIVATE ws2_32 iphlpapi dbghelp comdlg32 opengl32)
        # Newer mingw-w64 maps clock_gettime → clock_gettime64 in libwinpthread.
        # Link it even when netplay code prefers Win32 clocks, so any residual
        # POSIX time refs (third-party / debug tools) resolve under -static.
        if(MINGW)
            target_link_libraries(${target} PRIVATE winpthread)
        endif()
    else()
        if(CMAKE_DL_LIBS)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        endif()
        # GL for gpu_gl_renderer.c. Do NOT hardcode OpenGL::GL: FindOpenGL
        # reports OpenGL as found on a GLVND host that has libOpenGL.so but no
        # legacy libGL.so / glx.h (Steam Deck), yet never creates that target —
        # which then fails at generate time. recomp_resolve_gl() picks whatever
        # the host actually has; see recomp-ui/cmake/recomp_gl.cmake.
        if(NOT COMMAND recomp_resolve_gl
           AND RECOMP_UI_ROOT AND EXISTS "${RECOMP_UI_ROOT}/cmake/recomp_gl.cmake")
            include("${RECOMP_UI_ROOT}/cmake/recomp_gl.cmake")
        endif()
        if(COMMAND recomp_resolve_gl)
            recomp_resolve_gl(_psx_gl_target)
            target_link_libraries(${target} PRIVATE ${_psx_gl_target})
        else()
            # No recomp-ui checkout (PSX_RECOMP_UI=OFF): same resolution inline.
            find_package(OpenGL)
            if(TARGET OpenGL::GL)
                target_link_libraries(${target} PRIVATE OpenGL::GL)
            elseif(TARGET OpenGL::OpenGL)
                target_link_libraries(${target} PRIVATE OpenGL::OpenGL)
            elseif(OPENGL_gl_LIBRARY)
                target_link_libraries(${target} PRIVATE "${OPENGL_gl_LIBRARY}")
            elseif(OPENGL_opengl_LIBRARY)
                target_link_libraries(${target} PRIVATE "${OPENGL_opengl_LIBRARY}")
            endif()
        endif()
        # Async lobby connect (psx_lobby_client.c) uses pthread on Unix.
        if(PSXRECOMP_HAS_LOBBY_CLIENT)
            find_package(Threads REQUIRED)
            target_link_libraries(${target} PRIVATE Threads::Threads)
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
    # Build Vulkan when its SDK tools are available so release binaries can offer
    # it without game projects opting in individually. This does not select the
    # runtime renderer: OpenGL remains the default in config_loader.h. Builders
    # can still use -DPSX_ENABLE_VULKAN=OFF to produce the inert stub explicitly.
    option(PSX_ENABLE_VULKAN "Build the Vulkan renderer backend when SDK tools are available" ON)
    if(PSX_ENABLE_VULKAN)
    # $VULKAN_SDK first; else find_path. Unset before find_path — an empty
    # normal _vk_inc makes find_path a no-op on modern CMake (Homebrew miss).
    set(_vk_inc "")
    if(DEFINED ENV{VULKAN_SDK})
        if(EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
            set(_vk_inc "$ENV{VULKAN_SDK}/Include")
        elseif(EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
            set(_vk_inc "$ENV{VULKAN_SDK}/include")
        endif()
    endif()
    if(NOT _vk_inc)
        unset(_vk_inc CACHE)
        unset(_vk_inc)
        find_path(_vk_inc vulkan/vulkan.h)
    endif()
    find_program(GLSLC_EXE NAMES glslc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    # Finding the header is not the same as being able to include it. The usual
    # miss is _vk_inc=/usr/include: CMake drops that directory from include
    # lists (adding it explicitly would disturb the system header order), so
    # the -I never reaches the compiler, and a sysroot toolchain never had it
    # on the search path to begin with. Verify, then fall back to the inert
    # stub this block already knows how to build.
    set(_vk_stage "")
    if(_vk_inc AND GLSLC_EXE)
        _psx_header_compiles(_psx_vk_ok "vulkan/vulkan.h" INCLUDES "${_vk_inc}")
        if(NOT _psx_vk_ok)
            # Unreachable as given. Rather than give up the renderer, stage a
            # private include dir holding ONLY the Vulkan subtrees and try that:
            # it is not /usr/include, so CMake will emit it, and it pulls none
            # of the rest of the host include root ahead of the sysroot's own
            # libc headers -- which is what makes this safe where adding
            # ${_vk_inc} itself would not be. vk_video/ has to come along too;
            # vulkan_core.h includes the H.264/H.265 codec headers from it, so
            # staging vulkan/ alone still fails to compile.
            string(SHA256 _vk_stage_key "${_vk_inc}")
            string(SUBSTRING "${_vk_stage_key}" 0 12 _vk_stage_key)
            set(_vk_stage "${CMAKE_CURRENT_BINARY_DIR}/${target}_vkinc_${_vk_stage_key}")
            file(MAKE_DIRECTORY "${_vk_stage}")
            foreach(_vk_sub vulkan vk_video)
                if(EXISTS "${_vk_inc}/${_vk_sub}")
                    if(NOT EXISTS "${_vk_stage}/${_vk_sub}" AND
                       NOT IS_SYMLINK "${_vk_stage}/${_vk_sub}")
                        file(CREATE_LINK "${_vk_inc}/${_vk_sub}"
                             "${_vk_stage}/${_vk_sub}" SYMBOLIC COPY_ON_ERROR)
                    endif()
                endif()
            endforeach()
            _psx_header_compiles(_psx_vk_ok "vulkan/vulkan.h" INCLUDES "${_vk_stage}")
            if(_psx_vk_ok)
                message(STATUS
                    "Vulkan backend: ${_vk_inc} is not reachable directly; "
                    "staged ${_vk_stage} instead.")
            else()
                message(STATUS
                    "Vulkan backend: headers at ${_vk_inc} are not reachable from "
                    "${CMAKE_C_COMPILER} - gpu_vk_renderer.c builds as a software "
                    "stub. Set VULKAN_SDK to a copy the compiler can see to enable it.")
                set(_vk_inc "")
                set(_vk_stage "")
            endif()
        endif()
        unset(_psx_vk_ok)
    endif()
    if(_vk_inc AND GLSLC_EXE)
        message(STATUS "Vulkan backend: headers ${_vk_inc}, glslc ${GLSLC_EXE}")
        if(_vk_stage)
            target_include_directories(${target} PRIVATE "${_vk_stage}")
        else()
            target_include_directories(${target} PRIVATE "${_vk_inc}")
        endif()
        target_compile_definitions(${target} PRIVATE PSX_HAVE_VULKAN=1)
        # Compile every shader under runtime/shaders/ to SPIR-V (glslc) and embed
        # them into one generated header (vk_shaders_spv.h) of uint32_t arrays, so
        # gpu_vk_renderer.c creates shader modules with no runtime file deps.
        # Resolve the native interpreter behind the Windows Python launcher.
        # Invoking py.exe with a .py file can honor that file's /usr/bin/env
        # shebang and accidentally select an unrelated MSYS Python, which cannot
        # open the native absolute paths emitted by Windows CMake/Ninja.
        if(WIN32 AND NOT PSX_PYTHON)
            find_program(_psx_python_launcher NAMES py)
            if(_psx_python_launcher)
                execute_process(
                    COMMAND "${_psx_python_launcher}" -3 -c
                            "import sys; print(sys.executable)"
                    OUTPUT_VARIABLE _psx_native_python
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                if(EXISTS "${_psx_native_python}")
                    set(PSX_PYTHON "${_psx_native_python}" CACHE FILEPATH
                        "Python 3 interpreter used by runtime build tools")
                endif()
            endif()
        endif()
        if(NOT PSX_PYTHON)
            find_program(PSX_PYTHON NAMES python python3)
        endif()
        if(NOT PSX_PYTHON)
            message(FATAL_ERROR
                "Vulkan shader embedding requires Python 3 (py/python/python3)")
        endif()
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

    # Prefer BSS for zero-init data. MinGW+LTO has emitted multi‑MiB rings into
    # .rdata as stored zeros (~150MiB MotK .exe bloat); pair with PSX_BSS on the
    # largest arrays (see runtime/include/psx_bss.h).
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-fzero-initialized-in-bss>
            $<$<COMPILE_LANGUAGE:CXX>:-fzero-initialized-in-bss>)
    endif()

    if(MINGW)
        target_link_options(${target} PRIVATE -Wl,--stack,67108864)
        # No console window in Release MinGW builds.
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-mwindows>)
        if(PSX_STATIC_RUNTIME)
            # Fold the GCC / C++ / winpthread runtimes into the exe so it
            # imports only Windows system DLLs (no libgcc_s_seh-1.dll /
            # libstdc++-6.dll dependency). Pairs with the static SDL link
            # above to make the exe fully self-contained. winpthread is
            # linked via target_link_libraries (see WIN32 block above).
            target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /GS- /guard:cf-)
        # Visual Studio project files cannot represent language-specific target
        # options on a mixed C/C++ target. Scope the experimental MSVC atomics
        # switch to the one C source that needs it instead.
        set_property(SOURCE ${PSXRECOMP_ROOT}/runtime/src/audio_trace.c
            APPEND PROPERTY COMPILE_OPTIONS /experimental:c11atomics)
        target_link_options(${target} PRIVATE /STACK:67108864,67108864 /GUARD:NO)
        # No console window in Release MSVC builds. /ENTRY keeps main() as
        # the entry point (not WinMain) while switching to the Windows subsystem.
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>)
    endif()

    # Packages may contain data-only VCDIFF recipes for deriving a private,
    # fingerprinted runtime image from the user's verified stock disc. The
    # decoder is supplied by the release builder and invoked only from this
    # fixed path; packages cannot provide or execute binaries.
    set(PSXRECOMP_XDELTA3_EXECUTABLE "" CACHE FILEPATH
        "Trusted xdelta3 executable copied beside runtime targets")
    if(PSXRECOMP_XDELTA3_EXECUTABLE)
        if(NOT EXISTS "${PSXRECOMP_XDELTA3_EXECUTABLE}")
            message(FATAL_ERROR
                "PSXRECOMP_XDELTA3_EXECUTABLE does not exist: "
                "${PSXRECOMP_XDELTA3_EXECUTABLE}")
        endif()
        if(WIN32)
            set(_psxmod_xdelta_name "xdelta3.exe")
        else()
            set(_psxmod_xdelta_name "xdelta3")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PSXRECOMP_XDELTA3_EXECUTABLE}"
                "$<TARGET_FILE_DIR:${target}>/${_psxmod_xdelta_name}"
            COMMENT "Staging trusted xdelta3 decoder for derived-disc mods"
            VERBATIM)
    endif()

    # PGXP variant auto-clone (docs/ENHANCEMENTS.md G1.10): with
    # -DPSX_PGXP_VARIANT=ON, every primary runtime target grows an
    # <exe>_pgxp sibling — the SAME arguments (same generated C, extras,
    # ports) compiled with -DPSX_PGXP=1 (see the PGXP option above). Done
    # HERE, at the end, by replaying ARGN recursively, so it works for
    # every caller — titles that call this function directly (Ape) and
    # the psxrecomp_add_game_runtime wrapper alike. Oracle/cosim targets
    # and the clone itself are excluded.
    if(NOT PSXRT_PGXP AND NOT PSXRT_ORACLE AND NOT PSXRT_COSIM)
        option(PSX_PGXP_VARIANT
            "Also build the <exe>_pgxp PGXP precision-shadowing variant" OFF)
        if(PSX_PGXP_VARIANT)
            psxrecomp_add_runtime_target(${target}-pgxp PGXP PGXP_CLONE ${ARGN})
        endif()
    endif()
endfunction()

# Compatibility for early v4 game projects that used the longer helper name.
function(psxrecomp_v4_add_runtime_target target)
    psxrecomp_add_runtime_target(${target} ${ARGN})
endfunction()

# ---------------------------------------------------------------------------
# High-level helper for title repos (setup-host + optional generated game C).
#
# Typical game CMakeLists.txt:
#   set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp")
#   include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
#   psxrecomp_add_game_runtime(psx-runtime
#     WINDOW_TITLE "My Game Recompiled"
#     GEN_MARKER "generated/SLUS_01234_dispatch.c"
#     GEN_FULL_GLOB "generated/SLUS_01234_full_*.c"
#     CODEGEN_SETUP_SOURCES codegen_setup.c
#     DEFAULT_GAME_CONFIG_PATH "game.toml"
#     LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
#     APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
#     MAX_PLAYERS 2
#     ENABLE_NETPLAY_IF_PRESENT
#     ENABLE_SETUP_WIZARD
#   )
#
# Remaining args are forwarded to psxrecomp_add_runtime_target.
# ---------------------------------------------------------------------------
function(psxrecomp_add_game_runtime target)
    set(options ENABLE_NETPLAY_IF_PRESENT ENABLE_SETUP_WIZARD)
    set(oneValueArgs
        GEN_MARKER
        GEN_FULL_FALLBACK
        VERSION_FILE
        CODEGEN_SETUP_INCLUDE_DIR
        NETPLAY_LOBBY_URL
    )
    set(multiValueArgs GEN_FULL_GLOB CODEGEN_SETUP_SOURCES)
    cmake_parse_arguments(PSXG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT PSXRECOMP_ROOT)
        message(FATAL_ERROR
            "psxrecomp_add_game_runtime: PSXRECOMP_ROOT is not set. "
            "Set it to the psxrecomp submodule path before including runtime.cmake.")
    endif()

    option(PSXRECOMP_FORCE_SETUP_HOST
        "Build without linking game C even if generated/ exists" OFF)
    option(PSXRECOMP_REQUIRE_GAME_C
        "Fail configure if generated game C is missing" OFF)

    # Legacy BPE option name (CI / docs may still pass -DBPE_FORCE_SETUP_HOST=ON).
    if(BPE_FORCE_SETUP_HOST)
        set(PSXRECOMP_FORCE_SETUP_HOST ON CACHE BOOL
            "Build without linking game C even if generated/ exists" FORCE)
    endif()
    if(BPE_REQUIRE_GAME_C)
        set(PSXRECOMP_REQUIRE_GAME_C ON CACHE BOOL
            "Fail configure if generated game C is missing" FORCE)
    endif()

    if(NOT PSXG_VERSION_FILE)
        set(PSXG_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    endif()
    if(NOT IS_ABSOLUTE "${PSXG_VERSION_FILE}")
        set(PSXG_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${PSXG_VERSION_FILE}")
    endif()

    # Lobby / release pin from VERSION when PSX_GAME_VERSION is unset.
    if(EXISTS "${PSXG_VERSION_FILE}")
        file(READ "${PSXG_VERSION_FILE}" _psxg_ver_raw)
        string(STRIP "${_psxg_ver_raw}" _psxg_release_version)
    else()
        set(_psxg_release_version "0.0.0")
    endif()
    set(PSX_GAME_VERSION "" CACHE STRING
        "Lobby release pin (empty = Release uses VERSION file, else dev)")
    if(PSX_GAME_VERSION STREQUAL "")
        get_property(_psxg_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(_psxg_is_multi)
            set(PSX_GAME_VERSION
                "$<IF:$<CONFIG:Release>,${_psxg_release_version},dev>")
        elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
            set(PSX_GAME_VERSION "${_psxg_release_version}")
        else()
            set(PSX_GAME_VERSION "dev")
        endif()
    endif()
    message(STATUS
        "psxrecomp game_version: ${PSX_GAME_VERSION} "
        "(from VERSION=${_psxg_release_version})")
    if(NOT "${PSX_GAME_VERSION}" STREQUAL ""
       AND NOT "${PSX_GAME_VERSION}" MATCHES "\\$<"
       AND NOT "${_psxg_release_version}" STREQUAL ""
       AND NOT "${_psxg_release_version}" STREQUAL "0.0.0"
       AND NOT "${PSX_GAME_VERSION}" STREQUAL "${_psxg_release_version}")
        message(WARNING
            "PSX_GAME_VERSION=${PSX_GAME_VERSION} differs from VERSION file "
            "(${_psxg_release_version}). Sticky CMakeCache after a VERSION bump "
            "causes netplay lobby list mismatches. Reconfigure with "
            "-DPSX_GAME_VERSION=${_psxg_release_version} or delete the build cache.")
    endif()

    # Use a game-root recomp-ui only when the caller did not select one.
    if((NOT RECOMP_UI_ROOT OR RECOMP_UI_ROOT STREQUAL "")
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/recomp_ui.cmake")
        set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui" CACHE PATH
            "Path to recomp-ui launcher" FORCE)
    endif()

    # Optional: enable netplay when recomp-net is present.
    if(PSXG_ENABLE_NETPLAY_IF_PRESENT)
        if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
            if(NOT RECOMP_NET_ROOT)
                set(RECOMP_NET_ROOT "${PSXRECOMP_ROOT}/lib/recomp-net" CACHE PATH
                    "Path to recomp-net; empty = auto-discover" FORCE)
            endif()
            if(NOT DEFINED PSX_NETPLAY)
                set(PSX_NETPLAY ON)
            endif()
        endif()
    endif()

    # ENABLE_NETPLAY_IF_PRESENT is nearly always a no-op, and used to be one
    # silently. By the time this function runs, runtime.cmake has already been
    # INCLUDED, and the include resolved recomp-net and decided whether the
    # netplay TUs compile for real or as stubs (see the PSX_NETPLAY block near
    # the top of this file). The option(PSX_NETPLAY ... OFF) up there has also
    # already made the `NOT DEFINED` test above false. So the flag can only
    # ever matter to a caller that set PSX_NETPLAY before the include -- which
    # is exactly what the scaffold's pre-include block does, and that block is
    # the real switch. Say so instead of leaving the caller to discover from a
    # stubbed binary that the flag they passed did nothing.
    if(PSXG_ENABLE_NETPLAY_IF_PRESENT AND NOT PSX_NETPLAY)
        if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
            message(WARNING
                "ENABLE_NETPLAY_IF_PRESENT was passed but PSX_NETPLAY is OFF, so "
                "netplay stays stubbed. This argument is read after "
                "runtime.cmake has already been included and wired netplay, so "
                "it cannot turn it on by itself. Set it BEFORE the include:\n"
                "  if(EXISTS \"\${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt\")\n"
                "      set(PSX_NETPLAY ON CACHE BOOL \"\" FORCE)\n"
                "  endif()")
        else()
            message(WARNING
                "ENABLE_NETPLAY_IF_PRESENT was passed but recomp-net is not "
                "checked out (${PSXRECOMP_ROOT}/lib/recomp-net), so netplay "
                "stays stubbed. Run: git -C psxrecomp submodule update --init "
                "lib/recomp-net")
        endif()
    endif()

    # Title default lobby WebSocket URL (compile-time; env PSX_NET_LOBBY_URL wins).
    if(PSXG_NETPLAY_LOBBY_URL)
        set(PSX_NET_LOBBY_DEFAULT_URL "${PSXG_NETPLAY_LOBBY_URL}" CACHE STRING
            "Compile-time default lobby URL (ws://host:port)" FORCE)
    endif()

    # Optional: advertise first-run wizard + Generate & rebuild.
    # Prefer setting -DPSX_SETUP_WIZARD=ON before include(runtime.cmake) so the
    # option() default does not stick OFF in an existing cache; this helper
    # still forces ON when the title lists ENABLE_SETUP_WIZARD.
    if(PSXG_ENABLE_SETUP_WIZARD)
        set(PSX_SETUP_WIZARD ON CACHE BOOL
            "Advertise first-run setup wizard + Generate & rebuild in recomp-ui"
            FORCE)
    endif()

    # Setup-host CI (-DPSXRECOMP_FORCE_SETUP_HOST=ON) without the wizard ships a
    # zip that never opens first-run / Generate & rebuild (BPE regression).
    if(PSXRECOMP_FORCE_SETUP_HOST AND NOT PSX_SETUP_WIZARD)
        message(FATAL_ERROR
            "PSXRECOMP_FORCE_SETUP_HOST=ON requires PSX_SETUP_WIZARD=ON.\n"
            "Add ENABLE_SETUP_WIZARD to psxrecomp_add_game_runtime(...), and/or:\n"
            "  set(PSX_SETUP_WIZARD ON CACHE BOOL \"…\" FORCE)\n"
            "before include(runtime.cmake), and/or pass -DPSX_SETUP_WIZARD=ON\n"
            "on the cmake command line (setup-release CI does this).")
    endif()

    if(NOT PSXG_GEN_MARKER)
        message(FATAL_ERROR
            "psxrecomp_add_game_runtime: GEN_MARKER is required "
            "(e.g. generated/SLUS_01234_dispatch.c)")
    endif()
    if(NOT IS_ABSOLUTE "${PSXG_GEN_MARKER}")
        set(_psxg_marker "${CMAKE_CURRENT_SOURCE_DIR}/${PSXG_GEN_MARKER}")
    else()
        set(_psxg_marker "${PSXG_GEN_MARKER}")
    endif()

    set(_psxg_has_game_c FALSE)
    if(EXISTS "${_psxg_marker}" AND NOT PSXRECOMP_FORCE_SETUP_HOST)
        set(_psxg_has_game_c TRUE)
    endif()

    if(NOT _psxg_has_game_c)
        set(PSXRECOMP_ALLOW_NO_BIOS ON CACHE BOOL
            "Allow runtime with no BIOS backends (setup host)" FORCE)
    endif()

    if(PSXRECOMP_REQUIRE_GAME_C AND NOT _psxg_has_game_c)
        message(FATAL_ERROR
            "PSXRECOMP_REQUIRE_GAME_C=ON but ${_psxg_marker} is missing. "
            "Generate with psxrecomp-game, or leave REQUIRE_GAME_C off to "
            "build the setup host.")
    endif()

    set(_psxg_extras)
    # psxrecomp_codegen_host.c unconditionally includes recomp_launcher.h, so
    # the title's setup host and the shared host implementation can only be
    # built alongside the recomp-ui submodule (PSX_RECOMP_UI).
    if(PSX_RECOMP_UI)
        list(APPEND _psxg_extras ${PSXG_CODEGEN_SETUP_SOURCES})
        list(APPEND _psxg_extras
            "${PSXRECOMP_ROOT}/host/psxrecomp_codegen_host.c")
    endif()

    set(_psxg_rt_args
        GAME_VERSION "${PSX_GAME_VERSION}"
        EXTRAS_SOURCES ${_psxg_extras}
        ${PSXG_UNPARSED_ARGUMENTS}
    )

    if(_psxg_has_game_c)
        message(STATUS
            "psxrecomp: linking generated game C (full runtime) — ${_psxg_marker}")
        set(_psxg_full_list "")
        foreach(_glob IN LISTS PSXG_GEN_FULL_GLOB)
            if(NOT IS_ABSOLUTE "${_glob}")
                set(_glob "${CMAKE_CURRENT_SOURCE_DIR}/${_glob}")
            endif()
            file(GLOB _hits "${_glob}")
            list(APPEND _psxg_full_list ${_hits})
        endforeach()
        if(NOT _psxg_full_list)
            if(PSXG_GEN_FULL_FALLBACK)
                if(NOT IS_ABSOLUTE "${PSXG_GEN_FULL_FALLBACK}")
                    set(_psxg_full_list
                        "${CMAKE_CURRENT_SOURCE_DIR}/${PSXG_GEN_FULL_FALLBACK}")
                else()
                    set(_psxg_full_list "${PSXG_GEN_FULL_FALLBACK}")
                endif()
            else()
                # Derive SLUS_*_full.c next to the dispatch marker.
                get_filename_component(_psxg_marker_dir "${_psxg_marker}" DIRECTORY)
                get_filename_component(_psxg_marker_name "${_psxg_marker}" NAME)
                string(REPLACE "_dispatch.c" "_full.c" _psxg_full_name
                    "${_psxg_marker_name}")
                set(_psxg_full_list "${_psxg_marker_dir}/${_psxg_full_name}")
            endif()
        endif()
        psxrecomp_add_runtime_target(${target}
            GAME_GENERATED_FULL_C ${_psxg_full_list}
            GAME_GENERATED_DISPATCH_C "${_psxg_marker}"
            ${_psxg_rt_args}
        )
        # psxrecomp_add_runtime_target auto-clones a ${target}-pgxp sibling
        # when PSX_PGXP_VARIANT is ON; fold it into the tail configuration
        # below so it gets the same game-codegen defines and includes.
        set(_psxg_targets ${target})
        if(TARGET ${target}-pgxp)
            list(APPEND _psxg_targets ${target}-pgxp)
        endif()
    else()
        message(STATUS
            "psxrecomp: setup host (no game C, no BIOS backends) — "
            "first-run Generate & rebuild")
        psxrecomp_add_runtime_target(${target} ${_psxg_rt_args})
        set(_psxg_targets ${target})
    endif()

    foreach(_psxg_t IN LISTS _psxg_targets)
        target_compile_definitions(${_psxg_t} PRIVATE PSX_HAS_GAME_CODEGEN=1)
        # Distinct from PSX_HAS_GAME_CODEGEN (which just means "generated game
        # C is linked"): this only fires when a codegen_setup.c-style host was
        # actually provided, since that file needs recomp-ui/launcher headers
        # that a --no-recomp-ui build does not have.
        if(PSX_RECOMP_UI AND PSXG_CODEGEN_SETUP_SOURCES)
            target_compile_definitions(${_psxg_t} PRIVATE PSX_HAS_CODEGEN_SETUP_HOST=1)
        endif()

        if(PSX_NET_LOBBY_DEFAULT_URL)
            # Stringify for C: PSX_NET_LOBBY_DEFAULT_URL="ws://..."
            target_compile_definitions(${_psxg_t} PRIVATE
                "PSX_NET_LOBBY_DEFAULT_URL=\"${PSX_NET_LOBBY_DEFAULT_URL}\"")
        endif()
    endforeach()

    # Include the portable codegen host. Do NOT add CMAKE_CURRENT_SOURCE_DIR
    # wholesale to -I: on case-insensitive macOS, #include <version> can pick
    # up the repo VERSION pin file. Title codegen_setup.h is found via
    # quote-include next to codegen_setup.c.
    set(_psxg_inc "${PSXRECOMP_ROOT}/host")
    if(PSXG_CODEGEN_SETUP_INCLUDE_DIR)
        list(APPEND _psxg_inc "${PSXG_CODEGEN_SETUP_INCLUDE_DIR}")
    endif()
    if(RECOMP_UI_ROOT)
        list(APPEND _psxg_inc "${RECOMP_UI_ROOT}/src")
    endif()
    foreach(_psxg_t IN LISTS _psxg_targets)
        target_include_directories(${_psxg_t} PRIVATE ${_psxg_inc})
    endforeach()
endfunction()
