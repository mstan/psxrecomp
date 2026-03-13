/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    RABBITIZER_DEF_INSTR_ID(
        cpu, , INVALID,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate}
    )

    #include "cpu/cpu_normal.inc"
    #include "cpu/cpu_special.inc"
    #include "cpu/cpu_regimm.inc"
    #include "cpu/cpu_cop0.inc"
    #include "cpu/cpu_cop0_bc0.inc"
    #include "cpu/cpu_cop0_tlb.inc"
    #include "cpu/cpu_cop1.inc"
    #include "cpu/cpu_cop1_bc1.inc"
    #include "cpu/cpu_cop1_fpu_s.inc"
    #include "cpu/cpu_cop1_fpu_d.inc"
    #include "cpu/cpu_cop1_fpu_w.inc"
    #include "cpu/cpu_cop1_fpu_l.inc"
    #include "cpu/cpu_cop2.inc"

#ifndef INSTRID_AVOID_USERDEF
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_00,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_01,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_02,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_03,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_04,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_05,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_06,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_07,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_08,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_09,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_10,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_11,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_12,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_13,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_14,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_15,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_16,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_17,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_18,
        .operands={0}
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, , USERDEF_19,
        .operands={0}
    )
#endif

    RABBITIZER_DEF_INSTR_ID(
        cpu, , MAX,
        .operands={0}
    )
