/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstructionR5900.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "common/Utils.h"
#include "common/RabbitizerConfig.h"
#include "instructions/RabbitizerRegister.h"

size_t RabbitizerOperandType_process_r5900_I(UNUSED const RabbitizerInstruction *self, char *dst,
                                             UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    if (RabbitizerConfig_Cfg.toolchainTweaks.gnuMode) {
        RABUTILS_BUFFER_CPY(dst, totalSize, "$I");
    } else {
        RABUTILS_BUFFER_CPY(dst, totalSize, "I");
    }

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_Q(UNUSED const RabbitizerInstruction *self, char *dst,
                                             UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    if (RabbitizerConfig_Cfg.toolchainTweaks.gnuMode) {
        RABUTILS_BUFFER_CPY(dst, totalSize, "$Q");
    } else {
        RABUTILS_BUFFER_CPY(dst, totalSize, "Q");
    }

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_R(UNUSED const RabbitizerInstruction *self, char *dst,
                                             UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    if (RabbitizerConfig_Cfg.toolchainTweaks.gnuMode) {
        RABUTILS_BUFFER_CPY(dst, totalSize, "$R");
    } else {
        RABUTILS_BUFFER_CPY(dst, totalSize, "R");
    }

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_ACC(UNUSED const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    if (RabbitizerConfig_Cfg.toolchainTweaks.gnuMode) {
        RABUTILS_BUFFER_CPY(dst, totalSize, "$ACC");
    } else {
        RABUTILS_BUFFER_CPY(dst, totalSize, "ACC");
    }

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_ACCxyzw(const RabbitizerInstruction *self, char *dst,
                                                   const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_ACC(self, dst, immOverride, immOverrideLength));

#if 0
    if (RAB_INSTR_R5900_GET_xyzw_x(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'x');
    }
    if (RAB_INSTR_R5900_GET_xyzw_y(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'y');
    }
    if (RAB_INSTR_R5900_GET_xyzw_z(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'z');
    }
    if (RAB_INSTR_R5900_GET_xyzw_w(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'w');
    }
#endif

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfs(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameR5900VF(RAB_INSTR_R5900_GET_vfs(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vft(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameR5900VF(RAB_INSTR_R5900_GET_vft(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfd(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameR5900VF(RAB_INSTR_R5900_GET_vfd(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfsxyzw(const RabbitizerInstruction *self, char *dst,
                                                   const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfs(self, dst, immOverride, immOverrideLength));

#if 0
    if (RAB_INSTR_R5900_GET_xyzw_x(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'x');
    }
    if (RAB_INSTR_R5900_GET_xyzw_y(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'y');
    }
    if (RAB_INSTR_R5900_GET_xyzw_z(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'z');
    }
    if (RAB_INSTR_R5900_GET_xyzw_w(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'w');
    }
#endif

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vftxyzw(const RabbitizerInstruction *self, char *dst,
                                                   const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vft(self, dst, immOverride, immOverrideLength));

#if 0
    if (RAB_INSTR_R5900_GET_xyzw_x(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'x');
    }
    if (RAB_INSTR_R5900_GET_xyzw_y(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'y');
    }
    if (RAB_INSTR_R5900_GET_xyzw_z(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'z');
    }
    if (RAB_INSTR_R5900_GET_xyzw_w(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'w');
    }
#endif

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfdxyzw(const RabbitizerInstruction *self, char *dst,
                                                   const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfd(self, dst, immOverride, immOverrideLength));

#if 0
    if (RAB_INSTR_R5900_GET_xyzw_x(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'x');
    }
    if (RAB_INSTR_R5900_GET_xyzw_y(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'y');
    }
    if (RAB_INSTR_R5900_GET_xyzw_z(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'z');
    }
    if (RAB_INSTR_R5900_GET_xyzw_w(self)) {
        RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, 'w');
    }
#endif

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfsn(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfs(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_n(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vftn(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vft(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_n(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfdn(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfd(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_n(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfsl(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfs(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_l(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vftl(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vft(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_l(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfdl(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfd(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_l(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfsm(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfs(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_m(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vftm(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vft(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_m(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vfdm(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                size_t immOverrideLength) {
    size_t totalSize = 0;
    uint8_t n;

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vfd(self, dst, immOverride, immOverrideLength));

    n = RAB_INSTR_R5900_GET_m(self);
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, "xyzw"[n]);

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vis(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameR5900VI(RAB_INSTR_R5900_GET_vis(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vit(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameR5900VI(RAB_INSTR_R5900_GET_vit(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vid(const RabbitizerInstruction *self, char *dst,
                                               UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;
    const char *reg = RabbitizerRegister_getNameR5900VI(RAB_INSTR_R5900_GET_vid(self));

    RABUTILS_BUFFER_CPY(dst, totalSize, reg);
    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vis_predecr(const RabbitizerInstruction *self, char *dst,
                                                       const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '-');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '-');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vis(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vit_predecr(const RabbitizerInstruction *self, char *dst,
                                                       const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '-');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '-');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vit(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vid_predecr(const RabbitizerInstruction *self, char *dst,
                                                       const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '-');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '-');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vid(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vis_postincr(const RabbitizerInstruction *self, char *dst,
                                                        const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vis(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '+');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '+');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vit_postincr(const RabbitizerInstruction *self, char *dst,
                                                        const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vit(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '+');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '+');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_vid_postincr(const RabbitizerInstruction *self, char *dst,
                                                        const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '(');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerOperandType_process_r5900_vid(self, dst, immOverride, immOverrideLength));

    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '+');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '+');
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ')');

    return totalSize;
}

size_t RabbitizerOperandType_process_r5900_immediate5(const RabbitizerInstruction *self, char *dst,
                                                      const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;
    int32_t number;

    if ((immOverride != NULL) && (immOverrideLength > 0)) {
        memcpy(dst, immOverride, immOverrideLength);
        return immOverrideLength;
    }

    number = RAB_INSTR_R5900_GET_imm5(self);
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

size_t RabbitizerOperandType_process_r5900_immediate15(const RabbitizerInstruction *self, char *dst,
                                                       const char *immOverride, size_t immOverrideLength) {
    size_t totalSize = 0;
    int32_t number;

    if ((immOverride != NULL) && (immOverrideLength > 0)) {
        memcpy(dst, immOverride, immOverrideLength);
        return immOverrideLength;
    }

    number = RAB_INSTR_R5900_GET_imm15(self) * 8;
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
