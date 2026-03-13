/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#ifndef RABBITIZER_INSTRUCTION_R3000GTE_HPP
#define RABBITIZER_INSTRUCTION_R3000GTE_HPP
#pragma once

#include "InstructionBase.hpp"


namespace rabbitizer {
    class InstructionR3000GTE : public InstructionBase {
    public:
        InstructionR3000GTE(uint32_t word, uint32_t vram);
        virtual ~InstructionR3000GTE();

        uint8_t GetR3000GTE_fakeOpcode() const;

        uint8_t GetR3000GTE_sf() const;
        uint8_t GetR3000GTE_mx() const;
        uint8_t GetR3000GTE_v() const;
        uint8_t GetR3000GTE_cv() const;
        uint8_t GetR3000GTE_lm() const;
    };
};


#endif
