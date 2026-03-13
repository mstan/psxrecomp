/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP rt, fs
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x00, mfc1,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_fs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .modifiesRt=true,
        .readsFs=true
    ) // Move Word From Floating-Point
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x01, dmfc1,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_fs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .modifiesRt=true,
        .readsFs=true
    ) // Doubleword Move From Floating-Point
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x04, mtc1,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_fs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .readsRt=true,
        .modifiesFs=true
    ) // Move Word to Floating-Point
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x05, dmtc1,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_fs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .readsRt=true,
        .modifiesFs=true
    ) // Doubleword Move To Floating-Point

    // OP rt, cop1cs
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x02, cfc1,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop1cs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .modifiesRt=true
    ) // Move Control Word from Floating-Point
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x06, ctc1,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop1cs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .readsRt=true
    ) // Move Control Word to Floating-Point
