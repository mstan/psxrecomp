# runner.cmake — include this from your game's CMakeLists.txt
# Requires PSXRECOMP_ROOT to be set to the psxrecomp directory.

option(INTERPRETER_ONLY "Run entire game through MIPS interpreter for function discovery" OFF)

set(PSXRECOMP_RUNNER_SOURCES
    ${PSXRECOMP_ROOT}/runner/src/launcher.c
    ${PSXRECOMP_ROOT}/runner/src/crc32.c
    ${PSXRECOMP_ROOT}/runner/src/main_runner.cpp
    ${PSXRECOMP_ROOT}/runner/src/runtime.c
    ${PSXRECOMP_ROOT}/runner/src/gte.cpp
    ${PSXRECOMP_ROOT}/runner/src/gpu_state.cpp
    ${PSXRECOMP_ROOT}/runner/src/gpu_interpreter.cpp
    ${PSXRECOMP_ROOT}/runner/src/opengl_renderer.cpp
    ${PSXRECOMP_ROOT}/runner/src/ps1_exe_parser.cpp
    ${PSXRECOMP_ROOT}/runner/src/overlay_detector.cpp
    ${PSXRECOMP_ROOT}/runner/src/overlay_manager.cpp
    ${PSXRECOMP_ROOT}/runner/src/iso_reader.cpp
    ${PSXRECOMP_ROOT}/runner/src/cdrom_stub.cpp
    ${PSXRECOMP_ROOT}/runner/src/xa_audio.cpp
    ${PSXRECOMP_ROOT}/runner/src/fmv_player.cpp
    ${PSXRECOMP_ROOT}/runner/src/spu.cpp
    ${PSXRECOMP_ROOT}/runner/src/input_script.cpp
    ${PSXRECOMP_ROOT}/runner/src/savestate.cpp
    ${PSXRECOMP_ROOT}/runner/src/debug_server.c
    ${PSXRECOMP_ROOT}/runner/src/func_logger.c
    ${PSXRECOMP_ROOT}/runner/src/interp_stub.c
)

set(PSXRECOMP_RUNNER_INCLUDE_DIRS
    ${PSXRECOMP_ROOT}/runner/include
    ${PSXRECOMP_ROOT}/runner/src
)

set(PSXRECOMP_GLAD_SOURCES
    ${PSXRECOMP_ROOT}/runner/lib/glad/src/glad.c
)

set(PSXRECOMP_GLAD_INCLUDE_DIRS
    ${PSXRECOMP_ROOT}/runner/lib/glad/include
)
