/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    /* Indexed by "function" operand */

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x01, RTPS,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x30, RTPT,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x29, DPCL,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x10, DPCS,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x2A, DPCT,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x11, INTPL,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x1E, NCS,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x20, NCT,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x13, NCDS,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x16, NCDT,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x1B, NCCS,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x3F, NCCT,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x14, CDP,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x1C, CC,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x06, NCLIP,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x2D, AVSZ3,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x2E, AVSZ4,
        .operands={0}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x12, MVMVA,
        .operands={RAB_OPERAND_r3000gte_sf, RAB_OPERAND_r3000gte_mx, RAB_OPERAND_r3000gte_v, RAB_OPERAND_r3000gte_cv, RAB_OPERAND_r3000gte_lm}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x28, SQR,
        .operands={RAB_OPERAND_r3000gte_sf}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x0C, OP,
        .operands={RAB_OPERAND_r3000gte_sf}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x3D, GPF,
        .operands={RAB_OPERAND_r3000gte_sf}
    )

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, 0x3E, GPL,
        .operands={RAB_OPERAND_r3000gte_sf}
    )
