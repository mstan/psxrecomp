/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

/*
    31--------26-25------21 -------------------------------5--------0
    |  = COP1   |  = S    |                               | function|
    ------6----------5-----------------------------------------6-----
    |--000--|--001--|--010--|--011--|--100--|--101--|--110--|--111--| lo
000 | ADD.S | SUB.S | MUL.S | DIV.S | SQRT.S| ABS.S | MOV.S | NEG.S |
001 |  ---  |  ---  |  ---  |  ---  |  ---  |  ---  |  ---  | ---   |
010 |  ---  |  ---  |  ---  |  ---  |  ---  |  ---  |RSQRT.S|  ---  |
011 | ADDA.S| SUBA.S| MULA.S|  ---  | MADD.S| MSUB.S|MADDA.S|MSUBA.S|
100 |  ---  | ---   |  ---  |  ---  | CVT.W |  ---  |  ---  |  ---  |
101 | MAX.S | MIN.S |  ---  |  ---  |  ---  |  ---  |  ---  |  ---  |
110 | C.F   | ---   | C.EQ  |  ---  | C.LT  |  ---  |  C.LE |  ---  |
111 | ---   | ---   |  ---  |  ---  |  ---  |  ---  |  ---  |  ---  |
 hi |-------|-------|-------|-------|-------|-------|-------|-------|
*/

    // The other instructions are implemented using the main CPU table

    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x04, c1__sqrt_s, c1,
        .operands={RAB_OPERAND_cpu_copraw}
    )

    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x16, rsqrt_s, rsqrt.s,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFd=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point Reciprocal SQuare RooT

    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x18, adda_s, adda.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFs=true,
        .readsFt=true
    ) // floating point ADD to Accumulator
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x19, suba_s, suba.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFs=true,
        .readsFt=true
    ) // floating point SUBtract to Accumulator
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x1A, mula_s, mula.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFs=true,
        .readsFt=true
    ) // floating point MULtiply to Accumulator

    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x1C, madd_s, madd.s,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFd=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point Multiply-ADD
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x1D, msub_s, msub.s,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFd=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point Multiply abd SUBtract
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x1E, madda_s, madda.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point Multiply-ADD Accumulator
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x1F, msuba_s, msuba.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point Multiply SUBtract from Accumulator

    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x28, max_s, max.s,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFd=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point MAXimum
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x29, min_s, min.s,
        .operands={RAB_OPERAND_cpu_fd, RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .modifiesFd=true,
        .readsFs=true,
        .readsFt=true
    ) // floating point MINimum

    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x34, c_lt_s, c.lt.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .readsFs=true,
        .readsFt=true
    )
    RABBITIZER_DEF_INSTR_ID_ALTNAME(
        r5900, 0x36, c_le_s, c.le.s,
        .operands={RAB_OPERAND_cpu_fs, RAB_OPERAND_cpu_ft},
        .isFloat=true,
        .readsFs=true,
        .readsFt=true
    )
