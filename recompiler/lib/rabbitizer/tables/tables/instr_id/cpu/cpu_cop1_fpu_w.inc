/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP fd, fs
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        cpu, 0x20, cvt_s_w, cvt.s.w,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .modifiesFd=true,
        .readsFs=true
    )
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        cpu, 0x21, cvt_d_w, cvt.d.w,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs},
        .instrType=RABBITIZER_INSTR_TYPE_UNKNOWN,
        .isFloat=true,
        .isDouble=true,
        .modifiesFd=true,
        .readsFs=true
    )
