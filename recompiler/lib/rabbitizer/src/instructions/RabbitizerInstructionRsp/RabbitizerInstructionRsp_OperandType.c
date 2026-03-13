/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstructionRsp.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "common/Utils.h"
#include "common/RabbitizerConfig.h"
#include "instructions/RabbitizerRegister.h"

size_t RabbitizerOperandType_process_rsp_rs(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspGpr(RAB_INSTR_GET_rs(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_rt(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspGpr(RAB_INSTR_GET_rt(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_rd(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspGpr(RAB_INSTR_GET_rd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_cop0d(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspCop0(RAB_INSTR_GET_cop0d(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_cop2t(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspCop2(RAB_INSTR_RSP_GET_cop2t(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_cop2cd(const RabbitizerInstruction *self, char *dst,
                                                UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspCop2Control(RAB_INSTR_RSP_GET_cop2cd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vs(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspVector(RAB_INSTR_RSP_GET_vs(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vt(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspVector(RAB_INSTR_RSP_GET_vt(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vd(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameRspVector(RAB_INSTR_RSP_GET_vd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vt_elementhigh(const RabbitizerInstruction *self, char *dst,
                                                        const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t element;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_vt(self, dst, immOverride, immOverrideLength));

    element = RAB_INSTR_RSP_GET_elementhigh(self);
    if (element != 0) {
        if ((element & 0x8) == 0x8) {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%i]", element & 7);
        } else if ((element & 0xC) == 0x4) {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%ih]", element & (~0xC));
        } else if ((element & 0xE) == 0x2) {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%iq]", element & (~0xE));
        } else {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%i]", element);
        }
    }
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vt_elementlow(const RabbitizerInstruction *self, char *dst,
                                                       const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_vt(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%i]", RAB_INSTR_RSP_GET_elementlow(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vd_de(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                               size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t de;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_vd(self, dst, immOverride, immOverrideLength));

    de = RAB_INSTR_RSP_GET_de(self);
    if ((de & 0x8) == 0x8) {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%i]", de & (~0x8));
    } else if ((de & 0xC) == 0x4) {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%ih]", de & (~0xC));
    } else if ((de & 0xE) == 0x2) {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%iq]", de & (~0xE));
    } else {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%i]", de);
    }
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_vs_index(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                  size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_vs(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "[%i]", RAB_INSTR_RSP_GET_index(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_offset_rs(const RabbitizerInstruction *self, char *dst,
                                                   const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%X", RabbitizerInstructionRsp_GetOffsetVector(self));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');
    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_rs(self, dst, immOverride, immOverrideLength));
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');
    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_immediate_base(const RabbitizerInstruction *self, char *dst,
                                                        const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

// TODO: consider making this a proper configuration
#if 0
    if ((immOverride != NULL) && (immOverrideLength > 0) || RAB_INSTR_GET_immediate(self) != 0) {
        RABUTILS_BUFFER_ADVANCE(dst, totalSize, RabbitizerOperandType_process_cpu_immediate(self, dst, immOverride, immOverrideLength));
    }
#endif
    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_cpu_immediate(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');
    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_rs(self, dst, immOverride, immOverrideLength));
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_rsp_maybe_rd_rs(const RabbitizerInstruction *self, char *dst,
                                                     const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t rd = RAB_INSTR_GET_rd(self);
    const RabbitizerRegisterDescriptor *regDescriptor = RabbitizerRegister_getDescriptor_Gpr(rd);

    if (!RabbitizerRegisterDescriptor_isRa(regDescriptor)) {
        RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                                RabbitizerOperandType_process_rsp_rd(self, dst, immOverride, immOverrideLength));

        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ',');
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ' ');
    }

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_rsp_rs(self, dst, immOverride, immOverrideLength));

    return totalSize;
}
