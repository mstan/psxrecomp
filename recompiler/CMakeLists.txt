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
add_executable(PSXRecomp
    src/ps1_exe_parser.cpp
    src/mips_decoder.cpp
    src/basic_block.cpp
    src/control_flow.cpp
    src/function_analysis.cpp
    src/code_generator.cpp
    src/annotations.cpp
    src/main_psx.cpp
)
target_include_directories(PSXRecomp PRIVATE
    include
    lib/rabbitizer/include
    lib/rabbitizer/cplusplus/include
    lib/fmt/include
    lib/toml11
    lib/ELFIO
)
target_link_libraries(PSXRecomp PRIVATE fmt rabbitizer)
