/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerRegister.h"

#include <assert.h>

#include "common/Utils.h"
#include "common/RabbitizerConfig.h"

#include "generated/Registers_Names_arrays.h"

const char *RabbitizerRegister_getNameGpr(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_GprO32_Names));

    switch (RabbitizerConfig_Cfg.regNames.gprAbiNames) {
        case RABBITIZER_ABI_NUMERIC:
            return RabbitizerRegister_GprO32_Names[regValue][0];

        default:
        case RABBITIZER_ABI_O32:
            return RabbitizerRegister_GprO32_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];

        case RABBITIZER_ABI_N32:
        case RABBITIZER_ABI_N64:
            return RabbitizerRegister_GprN32_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
    }
}

const char *RabbitizerRegister_getNameCop0(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop0_Names));

    return RabbitizerRegister_Cop0_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters &&
                                                           RabbitizerConfig_Cfg.regNames.vr4300Cop0NamedRegisters
                                                       ? 1
                                                       : 0];
}

const char *RabbitizerRegister_getNameCop1(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop1O32_Names));

    switch (RabbitizerConfig_Cfg.regNames.fprAbiNames) {
        default:
        case RABBITIZER_ABI_NUMERIC:
            return RabbitizerRegister_Cop1O32_Names[regValue][0];

        case RABBITIZER_ABI_O32:
            return RabbitizerRegister_Cop1O32_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];

        case RABBITIZER_ABI_N32:
            return RabbitizerRegister_Cop1N32_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];

        case RABBITIZER_ABI_N64:
            return RabbitizerRegister_Cop1N64_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
    }
}

const char *RabbitizerRegister_getNameCop1Control(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop1Control_Names));

    return RabbitizerRegister_Cop1Control_Names
        [regValue][RabbitizerConfig_Cfg.regNames.namedRegisters && RabbitizerConfig_Cfg.regNames.userFpcCsr ? 1 : 0];
}

const char *RabbitizerRegister_getNameCop2(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop2_Names));

    return RabbitizerRegister_Cop2_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const char *RabbitizerRegister_getNameRspGpr(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspGpr_Names));

    return RabbitizerRegister_RspGpr_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const char *RabbitizerRegister_getNameRspCop0(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspCop0_Names));

    return RabbitizerRegister_RspCop0_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters &&
                                                              RabbitizerConfig_Cfg.regNames.vr4300RspCop0NamedRegisters
                                                          ? 1
                                                          : 0];
}

const char *RabbitizerRegister_getNameRspCop2(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspCop2_Names));

    return RabbitizerRegister_RspCop2_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const char *RabbitizerRegister_getNameRspCop2Control(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspCop2Control_Names));

    return RabbitizerRegister_RspCop2Control_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const char *RabbitizerRegister_getNameRspVector(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspVector_Names));

    return RabbitizerRegister_RspVector_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const char *RabbitizerRegister_getNameR5900VF(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_R5900VF_Names));

    return RabbitizerRegister_R5900VF_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const char *RabbitizerRegister_getNameR5900VI(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_R5900VI_Names));

    return RabbitizerRegister_R5900VI_Names[regValue][RabbitizerConfig_Cfg.regNames.namedRegisters ? 1 : 0];
}

const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_Gpr(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_GprO32_Names));

    switch (RabbitizerConfig_Cfg.regNames.gprAbiNames) {
        default:
        case RABBITIZER_ABI_NUMERIC:
        case RABBITIZER_ABI_O32:
            return &RabbitizerRegister_GprO32_Descriptors[regValue];

        case RABBITIZER_ABI_N32:
        case RABBITIZER_ABI_N64:
            return &RabbitizerRegister_GprN32_Descriptors[regValue];
    }
}

const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_Cop0(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop0_Names));

    return &RabbitizerRegister_Cop0_Descriptors[regValue];
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_Cop1(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop1O32_Names));

    switch (RabbitizerConfig_Cfg.regNames.fprAbiNames) {
        default:
        case RABBITIZER_ABI_NUMERIC:
        case RABBITIZER_ABI_O32:
            return &RabbitizerRegister_Cop1O32_Descriptors[regValue];

        case RABBITIZER_ABI_N32:
            return &RabbitizerRegister_Cop1N32_Descriptors[regValue];

        case RABBITIZER_ABI_N64:
            return &RabbitizerRegister_Cop1N64_Descriptors[regValue];
    }
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_Cop1Control(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop1Control_Names));

    return &RabbitizerRegister_Cop1Control_Descriptors[regValue];
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_Cop2(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_Cop2_Names));

    return &RabbitizerRegister_Cop2_Descriptors[regValue];
}

const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_RspGpr(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspGpr_Names));

    return &RabbitizerRegister_RspGpr_Descriptors[regValue];
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_RspCop0(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspCop0_Names));

    return &RabbitizerRegister_RspCop0_Descriptors[regValue];
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_RspCop2(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspCop2_Names));

    return &RabbitizerRegister_RspCop2_Descriptors[regValue];
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_RspCop2Control(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspCop2Control_Names));

    return &RabbitizerRegister_RspCop2Control_Descriptors[regValue];
}
const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_RspVector(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_RspVector_Names));

    return &RabbitizerRegister_RspVector_Descriptors[regValue];
}

const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_R5900VF(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_R5900VF_Names));

    return &RabbitizerRegister_R5900VF_Descriptors[regValue];
}

const RabbitizerRegisterDescriptor *RabbitizerRegister_getDescriptor_R5900VI(uint8_t regValue) {
    assert(regValue < ARRAY_COUNT(RabbitizerRegister_R5900VI_Names));

    return &RabbitizerRegister_R5900VI_Descriptors[regValue];
}
