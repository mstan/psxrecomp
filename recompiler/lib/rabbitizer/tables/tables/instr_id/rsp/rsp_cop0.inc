/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP rt, cop0d
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x00, mfc0,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .modifiesRt=true,
        .notEmittedByCompilers=true
    ) // Move word From CP0
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x04, mtc0,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_cop0d},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .readsRt=true,
        .notEmittedByCompilers=true
    ) // Move word to CP0
