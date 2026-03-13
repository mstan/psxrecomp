/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstruction.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "common/Utils.h"
#include "common/RabbitizerConfig.h"
#include "instructions/RabbitizerRegister.h"

size_t RabbitizerOperandType_process_cpu_rs(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameGpr(RAB_INSTR_GET_rs(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_rt(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameGpr(RAB_INSTR_GET_rt(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_rd(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameGpr(RAB_INSTR_GET_rd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_zero(UNUSED const RabbitizerInstruction *self, char *dst,
                                              UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameGpr(0);

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_cop0d(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop0(RAB_INSTR_GET_cop0d(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_fs(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop1(RAB_INSTR_GET_fs(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_ft(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop1(RAB_INSTR_GET_ft(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_fd(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop1(RAB_INSTR_GET_fd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_cop1cs(const RabbitizerInstruction *self, char *dst,
                                                UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop1Control(RAB_INSTR_GET_cop1cs(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_cop2t(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop2(RAB_INSTR_GET_cop2t(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_cop2cd(const RabbitizerInstruction *self, char *dst,
                                                UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameCop2(RAB_INSTR_GET_cop2cd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_sa(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

// TODO: consider making this a proper configuration
#if 0
    if (RAB_INSTR_GET_sa(self) < 10) {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_GET_sa(self));
    } else {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%x", RAB_INSTR_GET_sa(self));
    }
#endif
    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_GET_sa(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_op(const RabbitizerInstruction *self, char *dst,
                                            UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

// TODO: consider making this a proper configuration
#if 0
    if (RAB_INSTR_GET_op(self) < 10) {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_GET_op(self));
    } else {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%x", RAB_INSTR_GET_op(self));
    }
#endif
    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%02X", RAB_INSTR_GET_op(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_code(const RabbitizerInstruction *self, char *dst,
                                              UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    int code = RAB_INSTR_GET_code_upper(self);
    int lower = RAB_INSTR_GET_code_lower(self);

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", code);
    if (lower) {
        RABUTILS_BUFFER_SPRINTF(dst, totalSize, ", %i", lower);
    }

    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_code_lower(const RabbitizerInstruction *self, char *dst,
                                                    UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    int code_lower = RAB_INSTR_GET_code_lower(self);

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", code_lower);

    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_copraw(const RabbitizerInstruction *self, char *dst,
                                                UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%X", RAB_INSTR_GET_copraw(self));

    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_label(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                               size_t immOverrideLength) {
    size_t totalSize = 0;

    if ((immOverride != NULL) && (immOverrideLength > 0)) {
        memcpy(dst, immOverride, immOverrideLength);
        return immOverrideLength;
    }

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "func_%06X", RabbitizerInstruction_getInstrIndexAsVram(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_immediate(const RabbitizerInstruction *self, char *dst,
                                                   const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;
    int32_t number;

    if ((immOverride != NULL) && (immOverrideLength > 0)) {
        memcpy(dst, immOverride, immOverrideLength);
        return immOverrideLength;
    }

    number = RabbitizerInstruction_getProcessedImmediate(self);
    if (RabbitizerConfig_Cfg.misc.omit0XOnSmallImm) {
        if (number > -10 && number < 10) {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", number);
            return totalSize;
        }
    }
    if (number < 0) {
        if (RabbitizerConfig_Cfg.misc.upperCaseImm) {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "-0x%X", -number);
        } else {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "-0x%x", -number);
        }
    } else {
        if (RabbitizerConfig_Cfg.misc.upperCaseImm) {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%X", number);
        } else {
            RABUTILS_BUFFER_SPRINTF(dst, totalSize, "0x%x", number);
        }
    }
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_branch_target_label(const RabbitizerInstruction *self, char *dst,
                                                             const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    if ((immOverride != NULL) && (immOverrideLength > 0)) {
        memcpy(dst, immOverride, immOverrideLength);
        return immOverrideLength;
    }

    RABUTILS_BUFFER_CPY(dst, totalSize, ". + 4 + (");
    RABUTILS_BUFFER_ADVANCE(dst, totalSize, RabbitizerOperandType_process_cpu_immediate(self, dst, NULL, 0));
    RABUTILS_BUFFER_CPY(dst, totalSize, " << 2)");
    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_immediate_base(const RabbitizerInstruction *self, char *dst,
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
                            RabbitizerOperandType_process_cpu_rs(self, dst, immOverride, immOverrideLength));
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_cpu_maybe_rd_rs(const RabbitizerInstruction *self, char *dst,
                                                     const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t rd = RAB_INSTR_GET_rd(self);
    const RabbitizerRegisterDescriptor *regDescriptor = RabbitizerRegister_getDescriptor_Gpr(rd);

    if (!RabbitizerRegisterDescriptor_isRa(regDescriptor)) {
        RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                                RabbitizerOperandType_process_cpu_rd(self, dst, immOverride, immOverrideLength));

        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ',');
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ' ');
    }

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_cpu_rs(self, dst, immOverride, immOverrideLength));

    return totalSize;
}
