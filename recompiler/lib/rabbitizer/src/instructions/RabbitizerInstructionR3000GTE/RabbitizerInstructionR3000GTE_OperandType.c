/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstructionR3000GTE.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "common/Utils.h"

size_t RabbitizerOperandType_process_r3000gte_sf(const struct RabbitizerInstruction *self, char *dst,
                                                 UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_R3000GTE_GET_sf(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_r3000gte_mx(const struct RabbitizerInstruction *self, char *dst,
                                                 UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_R3000GTE_GET_mx(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_r3000gte_v(const struct RabbitizerInstruction *self, char *dst,
                                                UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_R3000GTE_GET_v(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_r3000gte_cv(const struct RabbitizerInstruction *self, char *dst,
                                                 UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_R3000GTE_GET_cv(self));
    return totalSize;
}

size_t RabbitizerOperandType_process_r3000gte_lm(const struct RabbitizerInstruction *self, char *dst,
                                                 UNUSED const char *immOverride, UNUSED size_t immOverrideLength) {
    size_t totalSize = 0;

    RABUTILS_BUFFER_SPRINTF(dst, totalSize, "%i", RAB_INSTR_R3000GTE_GET_lm(self));
    return totalSize;
}
