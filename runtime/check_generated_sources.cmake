# check_generated_sources.cmake — fail the build with a HUMAN message when a
# game's recompiled C is missing, instead of letting the C compiler emit a raw
# "cc1: fatal error: <file>: No such file or directory".
#
# The runtime links generated/<serial>_{full,dispatch}.c, which are produced by
# a SEPARATE step (the recompiler tool) that a from-source builder runs before
# cmake --build. Those paths are marked GENERATED in CMake so `cmake configure`
# succeeds before the first generation — which means a skipped generation step
# is only discovered deep in the build as an inscrutable compiler error naming a
# file the user has no reason to know how to produce. This script is run as a
# build-time dependency of the runtime target (see psxrecomp_add_runtime_target)
# so the failure surfaces first, names the missing files, and states the fix.
#
# Invoked via:
#   cmake -DSOURCES_FILE=<path> -DTARGET=.. -DGAME_CONFIG=.. \
#         -DRECOMPILER=.. -DDOC=.. -P check_generated_sources.cmake
# or (small lists only; Windows CreateProcess is ~8191 chars):
#   cmake -DSOURCES=<;-list> ...

set(_sources "")
if(DEFINED SOURCES_FILE AND SOURCES_FILE)
    if(NOT EXISTS "${SOURCES_FILE}")
        message(FATAL_ERROR
            "check_generated_sources.cmake: SOURCES_FILE does not exist:\n"
            "  ${SOURCES_FILE}")
    endif()
    file(STRINGS "${SOURCES_FILE}" _sources)
elseif(DEFINED SOURCES)
    set(_sources ${SOURCES})
else()
    message(FATAL_ERROR
        "check_generated_sources.cmake requires SOURCES_FILE or SOURCES")
endif()

set(_missing "")
foreach(_src IN LISTS _sources)
    if(_src AND NOT EXISTS "${_src}")
        list(APPEND _missing "${_src}")
    endif()
endforeach()

if(NOT _missing)
    return()  # all recompiled sources present — nothing to say.
endif()

if(NOT GAME_CONFIG)
    set(GAME_CONFIG "game.toml")
endif()
if(NOT RECOMPILER)
    set(RECOMPILER "psxrecomp/recompiler/build/psxrecomp-game")
endif()
if(NOT DOC)
    set(DOC "psxrecomp/docs/BUILDING.md  (\"Build and run a game\")")
endif()
if(NOT TARGET)
    set(TARGET "the runtime")
endif()

string(REPLACE ";" "\n    " _missing_pretty "${_missing}")

message(FATAL_ERROR
    "\n"
    "=====================================================================\n"
    " Recompiled game code is MISSING — cannot build ${TARGET}.\n"
    "=====================================================================\n"
    "\n"
    " These files do not exist yet:\n"
    "    ${_missing_pretty}\n"
    "\n"
    " They are not shipped and not produced by this build. They are emitted\n"
    " by the recompiler tool, which you must run ONCE before building (and\n"
    " again whenever you change the disc/EXE or gen-time settings):\n"
    "\n"
    "    1. Build the recompiler tool (separate CMake tree, one time):\n"
    "         cmake -S psxrecomp/recompiler -B psxrecomp/recompiler/build \\\n"
    "               -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
    "         cmake --build psxrecomp/recompiler/build\n"
    "\n"
    "    2. Extract your game's PS-X EXE from your own disc image, then\n"
    "       generate the recompiled C (paths come from ${GAME_CONFIG}):\n"
    "         ${RECOMPILER} --config ${GAME_CONFIG}\n"
    "\n"
    "    3. Re-run the build.\n"
    "\n"
    " Full walkthrough: ${DOC}\n"
    "=====================================================================\n")
