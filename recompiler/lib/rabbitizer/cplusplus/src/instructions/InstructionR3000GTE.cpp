/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/InstructionR3000GTE.hpp"

#include "instructions/RabbitizerInstructionR3000GTE.h"

using namespace rabbitizer;

InstructionR3000GTE::InstructionR3000GTE(uint32_t word, uint32_t vram) : InstructionBase() {
    RabbitizerInstructionR3000GTE_init(&this->instr, word, vram);
    RabbitizerInstructionR3000GTE_processUniqueId(&this->instr);
}

InstructionR3000GTE::~InstructionR3000GTE() {
    RabbitizerInstructionR3000GTE_destroy(&this->instr);
}

uint8_t InstructionR3000GTE::GetR3000GTE_fakeOpcode() const {
    return RAB_INSTR_R3000GTE_GET_FAKE_OPCODE(&this->instr);
}

uint8_t InstructionR3000GTE::GetR3000GTE_sf() const {
    return RAB_INSTR_R3000GTE_GET_sf(&this->instr);
}

uint8_t InstructionR3000GTE::GetR3000GTE_mx() const {
    return RAB_INSTR_R3000GTE_GET_mx(&this->instr);
}
uint8_t InstructionR3000GTE::GetR3000GTE_v() const {
    return RAB_INSTR_R3000GTE_GET_v(&this->instr);
}
uint8_t InstructionR3000GTE::GetR3000GTE_cv() const {
    return RAB_INSTR_R3000GTE_GET_cv(&this->instr);
}
uint8_t InstructionR3000GTE::GetR3000GTE_lm() const {
    return RAB_INSTR_R3000GTE_GET_lm(&this->instr);
}
