# Shared psxrecomp runtime CMake helpers.
#
# Include this from either the framework runtime build or a sibling game
# project. SDL3 is the default; set -DPSX_SDL_BACKEND=SDL2 for the legacy
# backend.

if(NOT DEFINED PSXRECOMP_ROOT)
    get_filename_component(PSXRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

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
list(REMOVE_DUPLICATES _PSX_TOOLCHAIN_PREFIX_HINTS)

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
        set(_psx_sdl3_src "")
        if(DEFINED FETCHCONTENT_SOURCE_DIR_SDL3 AND
           NOT FETCHCONTENT_SOURCE_DIR_SDL3 STREQUAL "" AND
           EXISTS "${FETCHCONTENT_SOURCE_DIR_SDL3}/CMakeLists.txt")
            set(_psx_sdl3_src "${FETCHCONTENT_SOURCE_DIR_SDL3}")
        elseif(DEFINED ENV{PSX_SDL3_SOURCE_DIR} AND
               NOT "$ENV{PSX_SDL3_SOURCE_DIR}" STREQUAL "" AND
               EXISTS "$ENV{PSX_SDL3_SOURCE_DIR}/CMakeLists.txt")
            set(_psx_sdl3_src "$ENV{PSX_SDL3_SOURCE_DIR}")
            set(FETCHCONTENT_SOURCE_DIR_SDL3 "${_psx_sdl3_src}" CACHE PATH
                "Pre-fetched SDL3 source (skip download)" FORCE)
        endif()
        if(_psx_sdl3_src)
            message(STATUS
                "psxrecomp: using pre-fetched SDL3 source ${_psx_sdl3_src}")
        endif()
        FetchContent_Declare(SDL3
            URL
                "https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-3.4.10.tar.gz"
            URL_HASH
                "SHA256=12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"
            ${_psx_sdl3_timestamp_args})
        FetchContent_MakeAvailable(SDL3)
        unset(_psx_sdl3_src)
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
    ${PSXRECOMP_ROOT}/runtime/src/dma.c
    ${PSXRECOMP_ROOT}/runtime/src/mdec.c
    ${PSXRECOMP_ROOT}/runtime/src/timers.c
    ${PSXRECOMP_ROOT}/runtime/src/interrupts.c
    ${PSXRECOMP_ROOT}/runtime/src/frame_pacing.c
    ${PSXRECOMP_ROOT}/runtime/src/host_time.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_fiber.c
    ${PSXRECOMP_ROOT}/runtime/src/sio.c
    ${PSXRECOMP_ROOT}/runtime/src/sio1.c
    ${PSXRECOMP_ROOT}/runtime/src/sio1_runtime.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_link.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_link_shm.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_link_pair.c
    ${PSXRECOMP_ROOT}/runtime/src/dual_machine.c
    ${PSXRECOMP_ROOT}/runtime/src/memcard.c
    ${PSXRECOMP_ROOT}/runtime/src/debug_server.c
    ${PSXRECOMP_ROOT}/runtime/src/debug_trace_ranges.c
    ${PSXRECOMP_ROOT}/runtime/src/dirty_ram_interp.c
    ${PSXRECOMP_ROOT}/runtime/src/game_dispatch_compat.c
    ${PSXRECOMP_ROOT}/runtime/src/fntrace.c
    ${PSXRECOMP_ROOT}/runtime/src/text_xlate.cpp
    ${PSXRECOMP_ROOT}/runtime/src/tex_pack.cpp
    ${PSXRECOMP_ROOT}/runtime/src/parity_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/device_trace.c
    ${PSXRECOMP_ROOT}/runtime/src/boot_state.c
    ${PSXRECOMP_ROOT}/runtime/src/netplay_snap_ring.c
    ${PSXRECOMP_ROOT}/runtime/src/netplay_state_digest.c
    ${PSXRECOMP_ROOT}/runtime/src/netplay_ram_dirty.c
    ${PSXRECOMP_ROOT}/runtime/src/psx_cpu_pin.c
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
    ${PSXRECOMP_ROOT}/runtime/src/mod_builtin_ram.c
    ${PSXRECOMP_ROOT}/runtime/src/mod_builtin_pgxp.c
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
    FetchContent_Declare(psx_zlib
        URL
            "https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz"
        URL_HASH
            "SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
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

function(psxrecomp_add_runtime_target target)
    # PGXP: build this target's objects with -DPSX_PGXP=1 so the PGXP_*()
    # hook macros the emitter writes into ALL generated C become real calls
    # into the value-propagation engine (pgxp_hooks.h; ENHANCEMENTS.md G1.10),
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
    endif()
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${_psxrt_exe_name}")

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
                        "$ENV{RETCOMM_TOOLCHAIN_DIR}/bin"
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

    # Framework-owned mod catalog (loading speed, 8 MB RAM). These target game_id "*" and
    # are emulator features rather than per-disc content, so every game gets
    # them without carrying a copy of the manifests. Staged BEFORE the game's
    # own POST_BUILD copy so a title may still override an id if it ever needs
    # to; copy_directory merges rather than replacing the tree.
    if(EXISTS "${PSXRECOMP_ROOT}/mods/builtin/packages")
        add_custom_command(TARGET ${target} POST_BUILD
            # Clear first: copy_directory MERGES, so a mod deleted from source
            # would otherwise survive in the build output forever and keep
            # appearing on the Mods page (and inflate the release packagers'
            # catalog assertions). This runs before the game's own staging, so
            # both catalogs land on a clean slate.
            #
            # Scoped to mods/packages, NOT mods/: both catalogs own only a
            # packages/ subtree, while mods/state.toml is USER STATE written at
            # runtime (which mods are enabled, with which options). Wiping the
            # whole tree silently disabled every enabled mod on every rebuild --
            # 8 MB, widescreen, framerate all reverting off, repeatedly misread
            # as the mods themselves regressing.
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$<TARGET_FILE_DIR:${target}>/mods/packages"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PSXRECOMP_ROOT}/mods/builtin"
                "$<TARGET_FILE_DIR:${target}>/mods"
            COMMENT "Staging framework-owned mod catalog"
            VERBATIM)
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
        find_package(OpenGL)
        if(OpenGL_FOUND)
            target_link_libraries(${target} PRIVATE OpenGL::GL)
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
    # $VULKAN_SDK first; then headers inside CMAKE_SYSROOT (RetComM jammy pack);
    # else host find_path only when not using a sysroot. Host /usr/include is
    # invisible (and unsafe) under clang --sysroot=…/toolchains/…/sysroot —
    # that combination made Hub Linux builds fail with:
    #   vulkan/vulkan.h: file not found
    # after configure claimed "headers /usr/include".
    set(_vk_inc "")
    set(_vk_inc_from_sdk FALSE)
    if(DEFINED ENV{VULKAN_SDK})
        if(EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
            set(_vk_inc "$ENV{VULKAN_SDK}/Include")
            set(_vk_inc_from_sdk TRUE)
        elseif(EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
            set(_vk_inc "$ENV{VULKAN_SDK}/include")
            set(_vk_inc_from_sdk TRUE)
        endif()
    endif()
    if(NOT _vk_inc AND CMAKE_SYSROOT)
        if(EXISTS "${CMAKE_SYSROOT}/usr/include/vulkan/vulkan.h")
            set(_vk_inc "${CMAKE_SYSROOT}/usr/include")
        elseif(EXISTS "${CMAKE_SYSROOT}/include/vulkan/vulkan.h")
            set(_vk_inc "${CMAKE_SYSROOT}/include")
        endif()
    endif()
    if(NOT _vk_inc AND NOT CMAKE_SYSROOT)
        unset(_vk_inc CACHE)
        unset(_vk_inc)
        find_path(_vk_inc vulkan/vulkan.h)
    endif()
    # Last-chance: find_path may still run under a sysroot build if callers
    # cleared CMAKE_SYSROOT after option() — reject host paths that sit
    # outside the active sysroot unless they came from VULKAN_SDK.
    if(_vk_inc AND CMAKE_SYSROOT AND NOT _vk_inc_from_sdk)
        file(TO_CMAKE_PATH "${CMAKE_SYSROOT}" _psx_vk_sysroot)
        file(TO_CMAKE_PATH "${_vk_inc}" _psx_vk_inc_norm)
        string(FIND "${_psx_vk_inc_norm}" "${_psx_vk_sysroot}" _psx_vk_under)
        if(NOT _psx_vk_under EQUAL 0)
            message(STATUS
                "Vulkan backend: ignoring host headers ${_vk_inc} "
                "(outside CMAKE_SYSROOT=${CMAKE_SYSROOT}); "
                "install vulkan-headers into the sysroot or set VULKAN_SDK")
            set(_vk_inc "")
        endif()
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

    # PGXP variant auto-clone (ENHANCEMENTS.md G1.10): with
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

    # Prefer game-root recomp-ui (runtime.cmake also auto-discovers this).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/recomp_ui.cmake")
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

    set(_psxg_extras ${PSXG_CODEGEN_SETUP_SOURCES})
    list(APPEND _psxg_extras
        "${PSXRECOMP_ROOT}/host/psxrecomp_codegen_host.c")

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
