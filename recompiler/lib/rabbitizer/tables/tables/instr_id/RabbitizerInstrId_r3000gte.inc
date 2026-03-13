/* SPDX-FileCopyrightText: © 2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , INVALID,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN
    )

    #include "r3000gte/r3000gte_cop2_gte.inc"

#ifndef INSTRID_AVOID_USERDEF
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_00,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_01,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_02,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_03,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_04,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_05,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_06,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_07,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_08,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_09,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_10,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_11,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_12,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_13,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_14,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_15,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_16,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_17,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_18,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , USERDEF_19,
        .operands={0}
    )
#endif

    RABBITIZER_DEF_INSTR_ID(
        r3000gte, , MAX,
        .operands={0}
    )
