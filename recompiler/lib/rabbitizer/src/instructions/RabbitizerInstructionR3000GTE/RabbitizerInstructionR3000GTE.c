/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstructionR3000GTE.h"

void RabbitizerInstructionR3000GTE_init(RabbitizerInstruction *self, uint32_t word, uint32_t vram) {
    RabbitizerInstruction_init(self, word, vram);

    self->uniqueId = RABBITIZER_INSTR_ID_r3000gte_INVALID;
    self->descriptor = &RabbitizerInstrDescriptor_Descriptors[self->uniqueId];

    self->category = RABBITIZER_INSTRCAT_R3000GTE;
}

void RabbitizerInstructionR3000GTE_destroy(RabbitizerInstruction *self) {
    RabbitizerInstruction_destroy(self);
}
