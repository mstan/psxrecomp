/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP rt, cop0d
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x00, mfc0,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .modifiesRt=true,
        .notEmittedByCompilers=true
    ) // Move word From CP0
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x01, dmfc0,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .modifiesRt=true,
        .notEmittedByCompilers=true
    ) // Doubleword Move From CP0
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x02, cfc0,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .modifiesRt=true,
        .notEmittedByCompilers=true
    ) // Move control word From CP0
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x04, mtc0,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .readsRt=true,
        .notEmittedByCompilers=true
    ) // Move word to CP0
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x05, dmtc0,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .readsRt=true,
        .notEmittedByCompilers=true
    ) // Doubleword Move To CP0
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x06, ctc0,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .readsRt=true,
        .notEmittedByCompilers=true
    ) // Move control word To CP0
