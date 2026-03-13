/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    RABBITIZER_DEF_REG(
        GPR_N32, zero,  0,
        .isZero=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, at,    1,
        .isClobberedByFuncCall=true,
        .isAt=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, v0,    2,
        .isClobberedByFuncCall=true,
        .isReturnReg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, v1,    3,
        .isClobberedByFuncCall=true,
        .isReturnReg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a0,    4,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a1,    5,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a2,    6,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a3,    7,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a4,    8,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a5,    9,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a6,   10,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, a7,   11,
        .isClobberedByFuncCall=true,
        .isArg=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, t0,   12,
        .isClobberedByFuncCall=true,
        .isTemp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, t1,   13,
        .isClobberedByFuncCall=true,
        .isTemp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, t2,   14,
        .isClobberedByFuncCall=true,
        .isTemp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, t3,   15,
        .isClobberedByFuncCall=true,
        .isTemp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s0,   16,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s1,   17,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s2,   18,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s3,   19,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s4,   20,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s5,   21,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s6,   22,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, s7,   23,
        .isSaved=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, t8,   24,
        .isClobberedByFuncCall=true,
        .isTemp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, t9,   25,
        .isClobberedByFuncCall=true,
        .isTemp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, k0,   26,
        .isKernel=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, k1,   27,
        .isKernel=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, gp,   28,
        .isGp=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, sp,   29,
        .isStackPointer=true
    )
    RABBITIZER_DEF_REG(
        GPR_N32, fp,   30,
        0
    )
    RABBITIZER_DEF_REG(
        GPR_N32, ra,   31,
        .isClobberedByFuncCall=true,
        .isRa=true
    )
