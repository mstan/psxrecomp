cmake_minimum_required(VERSION 3.20)
project(PSXRecomp C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(lib/fmt)

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
if(MINGW OR WIN32)
    target_link_options(psxrecomp-game PRIVATE -Wl,--stack,16777216)
endif()

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
if(MINGW OR WIN32)
    target_link_options(PSXRecomp PRIVATE -Wl,--stack,16777216)
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
    src/main_bios.cpp
)
target_include_directories(psxrecomp-bios PRIVATE
    include
    src
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
)
target_link_libraries(psxrecomp-bios PRIVATE fmt rabbitizer)
if(MINGW OR WIN32)
    target_link_options(psxrecomp-bios PRIVATE -Wl,--stack,16777216)
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
