/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstruction.h"

#include <assert.h>

#include "generated/instrOpercandCallbacks_array.h"

size_t RabbitizerOperandType_getBufferSize(RabbitizerOperandType operand, const RabbitizerInstruction *instr,
                                           size_t immOverrideLength) {
    char auxBuffer[0x100] = { 0 };
    char immOverride[0x100] = { 0 };
    OperandCallback callback;

    assert(operand > RAB_OPERAND_ALL_INVALID);
    assert(operand < RAB_OPERAND_ALL_MAX);

    callback = instrOpercandCallbacks[operand];
    assert(callback != NULL);

    return callback(instr, auxBuffer, immOverride, immOverrideLength);
}

size_t RabbitizerInstruction_getSizeForBufferOperandsDisasm(const RabbitizerInstruction *self,
                                                            size_t immOverrideLength) {
    size_t totalSize = 0;

    for (size_t i = 0;
         i < ARRAY_COUNT(self->descriptor->operands) && self->descriptor->operands[i] != RAB_OPERAND_ALL_INVALID; i++) {
        if (i != 0) {
            totalSize += 2;
        }

        totalSize += RabbitizerOperandType_getBufferSize(self->descriptor->operands[i], self, immOverrideLength);
    }

    return totalSize;
}

size_t RabbitizerOperandType_disassemble(RabbitizerOperandType operand, const RabbitizerInstruction *instr, char *dst,
                                         const char *immOverride, size_t immOverrideLength) {
    OperandCallback callback;

    assert(operand > RAB_OPERAND_ALL_INVALID);
    assert(operand < RAB_OPERAND_ALL_MAX);

    callback = instrOpercandCallbacks[operand];
    assert(callback != NULL);
    return callback(instr, dst, immOverride, immOverrideLength);
}

size_t RabbitizerInstruction_disassembleOperands(const RabbitizerInstruction *self, char *dst, const char *immOverride,
                                                 size_t immOverrideLength) {
    size_t totalSize = 0;

    for (size_t i = 0;
         i < ARRAY_COUNT(self->descriptor->operands) && self->descriptor->operands[i] != RAB_OPERAND_ALL_INVALID; i++) {
        if (i != 0) {
            RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ',');
            RABUTILS_BUFFER_WRITE_CHAR(dst, totalSize, ' ');
        }

        RABUTILS_BUFFER_ADVANCE(dst, totalSize,
                                RabbitizerOperandType_disassemble(self->descriptor->operands[i], self, dst, immOverride,
                                                                  immOverrideLength));
    }

    *dst = '\0';
    return totalSize;
}
