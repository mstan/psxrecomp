/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x01, tlbr,
        .operands={0},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .notEmittedByCompilers=true
    ) // Read Indexed TLB Entry
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x02, tlbwi,
        .operands={0},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .notEmittedByCompilers=true
    ) // Write Indexed TLB Entry

    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x06, tlbwr,
        .operands={0},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN
    ) // Write Random TLB Entry

    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x08, tlbp,
        .operands={0},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .notEmittedByCompilers=true
    ) // Probe TLB for Matching Entry

    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x18, eret,
        .operands={0},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .notEmittedByCompilers=true
    ) // Return from Exception
