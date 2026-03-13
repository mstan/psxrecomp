/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstructionR3000GTE.h"
#include "common/RabbitizerConfig.h"
#include "stdio.h"

#define RABBITIZER_DEF_INSTR_ID(prefix, caseBits, name, ...)    \
    case (caseBits):                                            \
        self->uniqueId = RABBITIZER_INSTR_ID_##prefix##_##name; \
        break;
#define RABBITIZER_DEF_INSTR_ID_ALTNAME(prefix, caseBits, name, altname, ...) \
    RABBITIZER_DEF_INSTR_ID(prefix, caseBits, name, __VA_ARGS__)

void RabbitizerInstructionR3000GTE_processUniqueId_Normal(RabbitizerInstruction *self) {
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_NORMAL;

    RabbitizerInstruction_processUniqueId_Normal(self);
}

void RabbitizerInstructionR3000GTE_processUniqueId_Special(RabbitizerInstruction *self) {
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_SPECIAL;

    RabbitizerInstruction_processUniqueId_Special(self);
}

void RabbitizerInstructionR3000GTE_processUniqueId_Regimm(RabbitizerInstruction *self) {
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_REGIMM;

    RabbitizerInstruction_processUniqueId_Regimm(self);
}

void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor0(RabbitizerInstruction *self) {
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_COP0;

    RabbitizerInstruction_processUniqueId_Coprocessor0(self);
}

void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor1(RabbitizerInstruction *self) {
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_COP1;

    RabbitizerInstruction_processUniqueId_Coprocessor1(self);
}

void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor2_gte(RabbitizerInstruction *self) {
    uint32_t function = RAB_INSTR_GET_function(self);

    self->_mandatorybits = RAB_INSTR_PACK_function(self->_mandatorybits, function);
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_COP2_GTE;

    // GTE instructions are weird
    self->_mandatorybits =
        RAB_INSTR_R3000GTE_PACK_FAKE_OPCODE(self->_mandatorybits, RAB_INSTR_R3000GTE_GET_FAKE_OPCODE(self));
    self->_mandatorybits = RAB_INSTR_R3000GTE_PACK_sf(self->_mandatorybits, RAB_INSTR_R3000GTE_GET_sf(self));
    self->_mandatorybits = RAB_INSTR_R3000GTE_PACK_mx(self->_mandatorybits, RAB_INSTR_R3000GTE_GET_mx(self));
    self->_mandatorybits = RAB_INSTR_R3000GTE_PACK_v(self->_mandatorybits, RAB_INSTR_R3000GTE_GET_v(self));
    self->_mandatorybits = RAB_INSTR_R3000GTE_PACK_cv(self->_mandatorybits, RAB_INSTR_R3000GTE_GET_cv(self));
    self->_mandatorybits = RAB_INSTR_R3000GTE_PACK_lm(self->_mandatorybits, RAB_INSTR_R3000GTE_GET_lm(self));

    switch (function) {
#include "tables/instr_id/r3000gte/r3000gte_cop2_gte.inc"
    }
}

void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor2(RabbitizerInstruction *self) {
    uint8_t fmt = RAB_INSTR_GET_fmt(self);
    bool fetchDescriptor = true;

    self->_mandatorybits = RAB_INSTR_PACK_fmt(self->_mandatorybits, fmt);
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_COP2;

    switch (fmt) {
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
            RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor2_gte(self);
            break;

        default:
            RabbitizerInstruction_processUniqueId_Coprocessor2(self);
            fetchDescriptor = false;
            break;
    }

    if (fetchDescriptor) {
        self->descriptor = &RabbitizerInstrDescriptor_Descriptors[self->uniqueId];
    }
}

#undef RABBITIZER_DEF_INSTR_ID
#undef RABBITIZER_DEF_INSTR_ID_ALTNAME

void RabbitizerInstructionR3000GTE_processUniqueId(RabbitizerInstruction *self) {
    uint32_t opcode = RAB_INSTR_GET_opcode(self);

    self->_mandatorybits = RAB_INSTR_PACK_opcode(self->_mandatorybits, opcode);
    self->instrIdType = RAB_INSTR_ID_TYPE_R3000GTE_INVALID;

    switch (opcode) {
        default:
            RabbitizerInstructionR3000GTE_processUniqueId_Normal(self);
            break;
        case 0x00:
            RabbitizerInstructionR3000GTE_processUniqueId_Special(self);
            break;
        case 0x01:
            RabbitizerInstructionR3000GTE_processUniqueId_Regimm(self);
            break;
        case 0x10:
            RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor0(self);
            break;
        case 0x11:
            RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor1(self);
            break;
        case 0x12:
            RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor2(self);
            break;
    }
}
