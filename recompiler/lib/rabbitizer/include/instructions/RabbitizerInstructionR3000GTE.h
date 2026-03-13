/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_INSTRUCTION_R3000GTE_H
#define RABBITIZER_INSTRUCTION_R3000GTE_H
#pragma once

#include "RabbitizerInstruction.h"

#ifdef __cplusplus
extern "C" {
#endif



#define RAB_INSTR_R3000GTE_GET_FAKE_OPCODE(self)            (SHIFTR((self)->word, 20,  5))

#define RAB_INSTR_R3000GTE_GET_sf(self)                     (SHIFTR((self)->word, 19,  1))
#define RAB_INSTR_R3000GTE_GET_mx(self)                     (SHIFTR((self)->word, 17,  2))
#define RAB_INSTR_R3000GTE_GET_v(self)                      (SHIFTR((self)->word, 15,  2))
#define RAB_INSTR_R3000GTE_GET_cv(self)                     (SHIFTR((self)->word, 13,  2))
#define RAB_INSTR_R3000GTE_GET_lm(self)                     (SHIFTR((self)->word, 10,  1))


#define RAB_INSTR_R3000GTE_PACK_FAKE_OPCODE(word, value)    (BITREPACK((word), (value), 20,  5))

#define RAB_INSTR_R3000GTE_PACK_sf(word, value)             (BITREPACK((word), (value), 19,  1))
#define RAB_INSTR_R3000GTE_PACK_mx(word, value)             (BITREPACK((word), (value), 17,  2))
#define RAB_INSTR_R3000GTE_PACK_v(word, value)              (BITREPACK((word), (value), 15,  2))
#define RAB_INSTR_R3000GTE_PACK_cv(word, value)             (BITREPACK((word), (value), 13,  2))
#define RAB_INSTR_R3000GTE_PACK_lm(word, value)             (BITREPACK((word), (value), 10,  1))



NON_NULL(1)
void RabbitizerInstructionR3000GTE_init(RabbitizerInstruction *self, uint32_t word, uint32_t vram);
NON_NULL(1)
void RabbitizerInstructionR3000GTE_destroy(RabbitizerInstruction *self);

NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId_Normal(RabbitizerInstruction *self);
NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId_Special(RabbitizerInstruction *self);
NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId_Regimm(RabbitizerInstruction *self);
NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor0(RabbitizerInstruction *self);
NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor1(RabbitizerInstruction *self);
NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId_Coprocessor2(RabbitizerInstruction *self);

NON_NULL(1)
void RabbitizerInstructionR3000GTE_processUniqueId(RabbitizerInstruction *self);


#ifdef __cplusplus
}
#endif

#endif
