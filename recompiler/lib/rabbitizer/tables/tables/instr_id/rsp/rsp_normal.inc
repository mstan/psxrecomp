/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP LABEL
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x02, j,
        .operands={RAB_OPERAND_cpu_label},
        .instrType=RABBITIZER_INSTR_TYPE_J,
        .isJump=true,
        .isJumpWithAddress=true
    ) // Jump
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x03, jal,
        .operands={RAB_OPERAND_cpu_label},
        .instrType=RABBITIZER_INSTR_TYPE_J,
        .isJump=true,
        .isJumpWithAddress=true,
        .doesLink=true
    ) // Jump And Link

    // OP rs, rt, IMM
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x04, beq,
        .operands={RAB_OPERAND_rsp_rs, RAB_OPERAND_rsp_rt, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .readsRs=true,
        .readsRt=true,
        .isBranch=true
    ) // Branch on EQual
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x05, bne,
        .operands={RAB_OPERAND_rsp_rs, RAB_OPERAND_rsp_rt, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .readsRs=true,
        .readsRt=true,
        .isBranch=true
    ) // Branch on Not Equal

    // OP rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x06, blez,
        .operands={RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .readsRs=true,
        .isBranch=true
    ) // Branch on Less than or Equal to Zero
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x07, bgtz,
        .operands={RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .readsRs=true,
        .isBranch=true
    ) // Branch on Greater Than Zero

    // OP rt, rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x08, addi,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .notEmittedByCompilers=true,
        .canBeLo=true
    ) // Add Immediate
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x09, addiu,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true
    ) // Add Immediate Unsigned Word
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x0A, slti,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true
    ) // Set on Less Than Immediate
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x0B, sltiu,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true
    ) // Set on Less Than Immediate Unsigned
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x0C, andi,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .readsRs=true
    ) // And Immediate
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x0D, ori,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true
    ) // Or Immediate
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x0E, xori,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .readsRs=true
    ) // eXclusive OR Immediate

    // OP rt, IMM
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x0F, lui,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .canBeHi=true
    ) // Load Upper Immediate

    // OP rt, IMM(base)
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x20, lb,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_BYTE
    ) // Load Byte
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x21, lh,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_SHORT
    ) // Load Halfword
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x23, lw,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_WORD
    ) // Load Word
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x24, lbu,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_BYTE,
        .doesUnsignedMemoryAccess=true
    ) // Load Byte Insigned
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x25, lhu,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_SHORT,
        .doesUnsignedMemoryAccess=true
    ) // Load Halfword Unsigned
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x28, sb,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_BYTE
    ) // Store Byte
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x29, sh,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_SHORT
    ) // Store Halfword
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x2B, sw,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_WORD
    ) // Store Word
    RABBITIZER_DEF_INSTR_ID(
        rsp, 0x33, pref,
        .operands={RAB_OPERAND_rsp_rt, RAB_OPERAND_rsp_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true
    ) // Prefetch


    // Pseudo-Instruction Unique IDs

    // OP IMM
    RABBITIZER_DEF_INSTR_ID(
        rsp, -0x03, b,
        .operands={RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true
    ) // Branch (unconditional)

    // OP rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        rsp, -0x04, beqz,
        .operands={RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .readsRs=true,
        .isBranch=true
    ) // Branch on EQual Zero
    RABBITIZER_DEF_INSTR_ID(
        rsp, -0x05, bnez,
        .operands={RAB_OPERAND_rsp_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .readsRs=true,
        .isBranch=true
    ) // Branch on Not Equal Zero
