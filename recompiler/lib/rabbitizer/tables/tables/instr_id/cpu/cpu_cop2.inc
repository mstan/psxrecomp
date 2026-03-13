/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP rt, cop2cd
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x00, mfc2,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop2cd},
        .modifiesRt=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x04, mtc2,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop2cd},
        .readsRt=true
    )

    // OP rt, cop2cd
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x02, cfc2,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop2cd},
        .modifiesRt=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x06, ctc2,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop2cd},
        .readsRt=true
    )
