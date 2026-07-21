cmake_minimum_required(VERSION 3.20)
project(PSXRecomp C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)

add_subdirectory(lib/fmt)

# Vendored fmt 9.1.0 enables its `consteval` format-string check on Apple
# clang via the `__cpp_consteval` clause in core.h, even though the same
# block tries to exclude Apple toolchains — and Apple clang 21 then rejects
# fmt's own literal format strings ("call to consteval function is not a
# constant expression"). Force FMT_CONSTEVAL empty to take fmt's
# non-consteval compile-time-check path. Scoped to non-MSVC so Windows is
# untouched.
if(NOT MSVC)
    target_compile_definitions(fmt PUBLIC FMT_CONSTEVAL=)
endif()

# Rabbitizer MIPS decoder
add_library(rabbitizer STATIC)
target_sources(rabbitizer PRIVATE
    lib/rabbitizer/cplusplus/src/analysis/LoPairingInfo.cpp
    lib/rabbitizer/cplusplus/src/analysis/RegistersTracker.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstrId.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstrIdType.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionBase.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionCpu.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionR3000GTE.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionR5900.cpp
    lib/rabbitizer/cplusplus/src/instructions/InstructionRsp.cpp
    lib/rabbitizer/src/analysis/RabbitizerLoPairingInfo.c
    lib/rabbitizer/src/analysis/RabbitizerRegistersTracker.c
    lib/rabbitizer/src/analysis/RabbitizerTrackedRegisterState.c
    lib/rabbitizer/src/common/RabbitizerConfig.c
    lib/rabbitizer/src/common/RabbitizerVersion.c
    lib/rabbitizer/src/common/Utils.c
    lib/rabbitizer/src/instructions/RabbitizerInstrCategory.c
    lib/rabbitizer/src/instructions/RabbitizerInstrDescriptor.c
    lib/rabbitizer/src/instructions/RabbitizerInstrId.c
    lib/rabbitizer/src/instructions/RabbitizerInstrIdType.c
    lib/rabbitizer/src/instructions/RabbitizerInstrSuffix.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionCpu/RabbitizerInstructionCpu_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR3000GTE/RabbitizerInstructionR3000GTE_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionR5900/RabbitizerInstructionR5900_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp_OperandType.c
    lib/rabbitizer/src/instructions/RabbitizerInstructionRsp/RabbitizerInstructionRsp_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Disassemble.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Examination.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_Operand.c
    lib/rabbitizer/src/instructions/RabbitizerInstruction/RabbitizerInstruction_ProcessUniqueId.c
    lib/rabbitizer/src/instructions/RabbitizerRegister.c
    lib/rabbitizer/src/instructions/RabbitizerRegisterDescriptor.c
)
target_include_directories(rabbitizer PRIVATE
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/rabbitizer/tables
)

# PSXRecomp — MIPS→C recompiler tool
set(PSXRECOMP_GAME_SOURCES
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/control_flow.cpp
    src/function_analysis.cpp
    src/code_generator.cpp
    src/annotations.cpp
    src/config_loader.cpp
    src/recompiler_patch.cpp
    src/main_psx.cpp
)

add_executable(psxrecomp-game ${PSXRECOMP_GAME_SOURCES})
target_include_directories(psxrecomp-game PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
    lib/ELFIO
)
target_link_libraries(psxrecomp-game PRIVATE fmt rabbitizer)
if(MINGW)
    target_link_options(psxrecomp-game PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-game PRIVATE /STACK:16777216)
endif()

# psxrecomp-toml — PS-X EXE → game.toml generator.
# Standalone tool that analyses a PS-X EXE and generates a complete game.toml
# config with auto-detected fields (load_address, entry_pc, text_size,
# stack_base) and optional JAL-target seed generation.
add_executable(psxrecomp-toml
    src/ps1_exe_parser.cpp
    src/main_toml.cpp
)
target_include_directories(psxrecomp-toml PRIVATE
    include
    lib/fmt/include
)
target_link_libraries(psxrecomp-toml PRIVATE fmt)

# psxrecomp — user-facing media-to-source project generator.
add_executable(psxrecomp
    src/main_cli.cpp
    src/ps1_exe_parser.cpp
    ../runtime/src/iso_reader.cpp
)
target_include_directories(psxrecomp PRIVATE
    include
    ../runtime/include
    lib/fmt/include
)
target_link_libraries(psxrecomp PRIVATE fmt)
if(MINGW)
    target_link_options(psxrecomp PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp PRIVATE /STACK:16777216)
endif()

# ---- baked codegen hash (stale-binary guard) --------------------------------
# Bake the SAME emitter-source hash the runtime folds into its overlay cache
# tag (runtime/runtime.cmake -> hash_codegen.cmake, shared canonical list in
# runtime/codegen_hash_sources.cmake) into THIS binary, exposed via
# `psxrecomp-game --codegen-hash`. compile_overlays.py compares it against the
# hash it stamps on the cache dir (from --runtime-include) and HARD-FAILS on
# mismatch. Closes the 2026-07-01 Tomba pause-menu wedge class: a stale
# recompiler binary emitting shards that get filed under the CURRENT cg tag
# (read tag == write tag, content stale — invisible to the tag alone).
set(PSXRECOMP_CODEGEN_HASH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${CMAKE_CURRENT_SOURCE_DIR}/../runtime/codegen_hash_sources.cmake)
set(_baked_hash_hdr ${CMAKE_CURRENT_BINARY_DIR}/psxrecomp_baked_codegen_hash.h)
add_custom_command(
    OUTPUT  ${_baked_hash_hdr}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_baked_hash_hdr}
            "-DSRCS=${PSXRECOMP_CODEGEN_HASH_SRCS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    DEPENDS ${PSXRECOMP_CODEGEN_HASH_SRCS}
            ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/hash_codegen.cmake
    COMMENT "Hashing codegen sources -> psxrecomp_baked_codegen_hash.h"
    VERBATIM)
add_custom_target(psxrecomp_baked_codegen_hash DEPENDS ${_baked_hash_hdr})
add_dependencies(psxrecomp-game psxrecomp_baked_codegen_hash)
# Force main_psx.cpp to recompile whenever the hash header content changes
# (same OBJECT_DEPENDS hardening as runtime.cmake — __has_include depfile gap).
set_source_files_properties(src/main_psx.cpp PROPERTIES OBJECT_DEPENDS ${_baked_hash_hdr})
target_include_directories(psxrecomp-game PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_executable(PSXRecomp ${PSXRECOMP_GAME_SOURCES})
target_include_directories(PSXRecomp PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
    lib/ELFIO
)
target_link_libraries(PSXRecomp PRIVATE fmt rabbitizer)

# Increase stack size for large binaries (1447+ functions)
if(MINGW)
    target_link_options(PSXRecomp PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(PSXRecomp PRIVATE /STACK:16777216)
endif()

# psxrecomp-bios — Phase 1a BIOS boot slice recompiler.
# Strict, fail-loud, ingests bios/SCPH1001.BIN as a flat ROM at 0xBFC00000.
# Independent of PSXRecomp's PS1-EXE entry point. Shares mips_decoder +
# ps1_exe_parser sources because mips_decoder.h depends on PS1Executable
# (used only by decode_executable, which psxrecomp-bios does not call).
add_executable(psxrecomp-bios
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/strict_translator.cpp
    src/bios_slice_walker.cpp
    src/function_discovery.cpp
    src/full_function_emitter.cpp
    src/config_loader.cpp
    src/recompiler_patch.cpp
    src/main_bios.cpp
)
target_include_directories(psxrecomp-bios PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
)
target_link_libraries(psxrecomp-bios PRIVATE fmt rabbitizer)
if(MINGW)
    target_link_options(psxrecomp-bios PRIVATE -Wl,--stack,16777216)
elseif(MSVC)
    target_link_options(psxrecomp-bios PRIVATE /STACK:16777216)
endif()

# Released Windows CLI archives must run without a separate MinGW install.
# Keep this opt-in so ordinary developer/debug builds retain their normal
# linker behavior on every platform.
option(PSXRECOMP_STATIC_CLI "Statically link MinGW runtime libraries into CLI tools" OFF)
if(PSXRECOMP_STATIC_CLI AND MINGW)
    foreach(_cli_target psxrecomp psxrecomp-game psxrecomp-toml psxrecomp-bios)
        target_link_options(${_cli_target} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
endif()

# L1 decoder conformance test — compares MipsDecoder against Rabbitizer
# oracle for every instruction in SCPH1001.BIN code regions.
add_executable(l1_decoder_test
    tests/l1_decoder_test.cpp
    src/mips_decoder.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(l1_decoder_test PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(l1_decoder_test PRIVATE fmt rabbitizer)

# L2 structural codegen test — verifies StrictTranslator output patterns
# for each MIPS instruction form.
add_executable(l2_structural_test
    tests/l2_structural_test.cpp
    src/mips_decoder.cpp
    src/ps1_exe_parser.cpp
    src/strict_translator.cpp
)
target_include_directories(l2_structural_test PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(l2_structural_test PRIVATE fmt rabbitizer)

if(BUILD_TESTING)
    add_executable(recompiler_patch_test
        tests/recompiler_patch_test.cpp
        src/ps1_exe_parser.cpp
        src/mips_decoder.cpp
        src/control_flow.cpp
        src/function_analysis.cpp
        src/code_generator.cpp
        src/annotations.cpp
        src/config_loader.cpp
        src/recompiler_patch.cpp
    )
    target_include_directories(recompiler_patch_test PRIVATE
        include
        src
        lib/rabbitizer/include
        lib/rabbitizer/cplusplus/include
        lib/fmt/include
        lib/toml11
        lib/ELFIO
    )
    target_link_libraries(recompiler_patch_test PRIVATE fmt rabbitizer)
    add_test(NAME recompiler_patch_test COMMAND recompiler_patch_test)

    add_executable(recompiler_patch_cxx17_header_test
        tests/recompiler_patch_cxx17_header_test.cpp
    )
    target_include_directories(recompiler_patch_cxx17_header_test PRIVATE src)
    set_target_properties(recompiler_patch_cxx17_header_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME recompiler_patch_cxx17_header_test
             COMMAND recompiler_patch_cxx17_header_test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME overlay_cross_page_delay_codegen
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_overlay_cross_page_delay_codegen.py
                    --recompiler $<TARGET_FILE:psxrecomp-game>)
    endif()
endif()

# Reachable main-EXE discovery test: parser-bound validation plus exact-entry
# analysis behavior for direct, indirect, seeded, and out-of-bound targets.
add_executable(reachable_discovery_test
    tests/reachable_discovery_test.cpp
    src/function_analysis.cpp
    src/ps1_exe_parser.cpp
)
target_include_directories(reachable_discovery_test PRIVATE
    include
    lib/fmt/include
)
target_link_libraries(reachable_discovery_test PRIVATE fmt)
