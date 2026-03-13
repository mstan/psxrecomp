/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstruction.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "common/RabbitizerConfig.h"

size_t RabbitizerInstruction_getSizeForBufferInstrDisasm(const RabbitizerInstruction *self, size_t immOverrideLength,
                                                         int extraLJust) {
    size_t totalSize = 0;
    size_t opcodeNameLength;

    opcodeNameLength = strlen(RabbitizerInstrId_getOpcodeName(self->uniqueId));

    totalSize += opcodeNameLength;

    totalSize += RabbitizerInstrSuffix_getSizeForBuffer(self, self->descriptor->instrSuffix);

    if (self->descriptor->operands[0] == RAB_OPERAND_ALL_INVALID) {
        // There are no operands
        return totalSize;
    }

    if (RabbitizerConfig_Cfg.misc.opcodeLJust > 0) {
        totalSize += RabbitizerConfig_Cfg.misc.opcodeLJust;
    }
    if (extraLJust > 0) {
        totalSize += extraLJust;
    }
    totalSize++;

    totalSize += RabbitizerInstruction_getSizeForBufferOperandsDisasm(self, immOverrideLength);

    return totalSize;
}

size_t RabbitizerInstruction_disassembleInstruction(const RabbitizerInstruction *self, char *dst,
                                                    const char *immOverride, size_t immOverrideLength, int extraLJust) {
    size_t totalSize = 0;
    const char *opcodeName = RabbitizerInstrId_getOpcodeName(self->uniqueId);

    RABUTILS_BUFFER_CPY(dst, totalSize, opcodeName);

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerInstrSuffix_processSuffix(self, dst, self->descriptor->instrSuffix));

    if (self->descriptor->operands[0] == RAB_OPERAND_ALL_INVALID) {
        // There are no operands
        *dst = '\0';
        return totalSize;
    }

    RABUTILS_BUFFER_ADVANCE(
        dst, totalSize,
        RabbitizerUtils_CharFill(dst, RabbitizerConfig_Cfg.misc.opcodeLJust + extraLJust - totalSize, ' '));
    RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ' ');

    RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                            RabbitizerInstruction_disassembleOperands(self, dst, immOverride, immOverrideLength));

    *dst = '\0';
    return totalSize;
}

size_t RabbitizerInstruction_getSizeForBufferDataDisasm(UNUSED const RabbitizerInstruction *self, int extraLJust) {
    size_t totalSize = 0;
    int tempLJust;

    totalSize += strlen(".word");

    tempLJust = RabbitizerConfig_Cfg.misc.opcodeLJust + extraLJust;
    tempLJust = RAB_MAX(tempLJust, 0);
    totalSize += tempLJust;

    totalSize += 11;
    return totalSize;
}

size_t RabbitizerInstruction_disassembleAsData(const RabbitizerInstruction *self, char *dst, int extraLJust) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_CPY(dst, totalSize, ".word");

    RABUTILS_BUFFER_ADVANCE(
        dst, totalSize,
        RabbitizerUtils_CharFill(dst, RabbitizerConfig_Cfg.misc.opcodeLJust + extraLJust - totalSize, ' '));

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, " 0x%08X", RabbitizerInstruction_getRaw(self));
    return totalSize;
}

bool RabbitizerInstruction_mustDisasmAsData(const RabbitizerInstruction *self) {
    if (RabbitizerConfig_Cfg.toolchainTweaks.sn64DivFix) {
        if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_break) {
            return true;
        }
    }

    if (RabbitizerConfig_Cfg.toolchainTweaks.gnuMode) {
        switch (self->uniqueId) {
            case RABBITIZER_INSTR_ID_cpu_trunc_w_s:
            case RABBITIZER_INSTR_ID_cpu_cvt_w_s:
                if (self->category == RABBITIZER_INSTRCAT_R5900) {
                    /**
                     * Due to the R5900's FPU being non properly complaint, the instruction cvt.w.s always behaves as
                     * trunc.w.s because EE can only do round-to-zero.
                     *
                     * Assemblers like GAS workaround this issue by decoding cvt.w.s as trunc.w.s, but other assemblers
                     * just use trunc.w.s and cvt.w.s as-is.
                     *
                     * Here's some reading about the binutils rationale:
                     * - https://sourceware.org/legacy-ml/binutils/2012-11/msg00360.html
                     * - https://sourceware.org/pipermail/binutils/2013-January/079863.html
                     *
                     * Because of this, building with GAS with the -march=r5900 flag produces:
                     * - trunc.w.s is built as the cvt.w.s instruction.
                     * - cvt.w.s errors complaining as not being supported by the processor.
                     *
                     * To ensure the produced disassembly will still match when built with GAS, we decode this two
                     * instructions as .word
                     */
                    return true;
                }
                break;
            case RABBITIZER_INSTR_ID_r5900_vclipw:
                // The vclipw instruction has variants that are undocumented (vclipw.xy, vclipw.z) and don't assemble in
                // gnu as
                return true;
            case RABBITIZER_INSTR_ID_r5900_vsqrt:
                // The vclipw instruction seems to be representable in multiple ways, and we only disassemble one of
                // them
                return true;

            default:
                break;
        }
    }

    if (!RabbitizerInstruction_isValid(self)) {
        return true;
    }
    return false;
}

size_t RabbitizerInstruction_getSizeForBuffer(const RabbitizerInstruction *self, size_t immOverrideLength,
                                              int extraLJust) {
    if (!RabbitizerInstruction_isImplemented(self) || RabbitizerInstruction_mustDisasmAsData(self)) {
        size_t totalSize = RabbitizerInstruction_getSizeForBufferDataDisasm(self, extraLJust);

        if (RabbitizerConfig_Cfg.misc.unknownInstrComment) {
            totalSize += 40;
            totalSize += 3; // " # "
            totalSize += RabbitizerInstruction_getSizeForBufferInstrDisasm(self, immOverrideLength, extraLJust);
            totalSize += strlen(" # 00000000"); // " # %08X"

            totalSize += strlen(" <InstrIdType: %s>");
            totalSize += strlen(RabInstrIdType_getName(self->instrIdType));
        }
        return totalSize;
    }

    return RabbitizerInstruction_getSizeForBufferInstrDisasm(self, immOverrideLength, extraLJust);
}

size_t RabbitizerInstruction_disassemble(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                         size_t immOverrideLength, int extraLJust) {
    assert(dst != NULL);

    if (!RabbitizerInstruction_isImplemented(self) || RabbitizerInstruction_mustDisasmAsData(self)) {
        size_t totalSize = 0;

        RABUTILS_BUFFER_ADVANCE(dst, totalSize, RabbitizerInstruction_disassembleAsData(self, dst, extraLJust));

        if (RabbitizerConfig_Cfg.misc.unknownInstrComment) {
            uint32_t validBits;

            RABUTILS_BUFFER_ADVANCE(dst, totalSize, RabbitizerUtils_CharFill(dst, 40 - totalSize, ' '));

            RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ' ');
            RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, '#');
            RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ' ');

            RABUTILS_BUFFER_ADVANCE(
                dst, totalSize,
                RabbitizerInstruction_disassembleInstruction(self, dst, immOverride, immOverrideLength, extraLJust));

            validBits = RabbitizerInstruction_getValidBits(self);

            RABUTILS_BUFFER_SPRINTF(dst, totalSize, " # %08X", ((~validBits) & self->word));

            RABUTILS_BUFFER_SPRINTF(dst, totalSize, " <InstrIdType: %s>", RabInstrIdType_getName(self->instrIdType));
        }

        return totalSize;
    }

    return RabbitizerInstruction_disassembleInstruction(self, dst, immOverride, immOverrideLength, extraLJust);
}
