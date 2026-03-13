/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP LABEL
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x02, j,
        .operands={RAB_OPERAND_cpu_label},
        .instrType=RABBITIZER_INSTR_TYPE_J,
        .isJump=true,
        .isJumpWithAddress=true
    ) // Jump
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x03, jal,
        .operands={RAB_OPERAND_cpu_label},
        .instrType=RABBITIZER_INSTR_TYPE_J,
        .isJump=true,
        .isJumpWithAddress=true,
        .doesLink=true
    ) // Jump And Link

    // OP rs, rt, IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x04, beq,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .readsRs=true,
        .readsRt=true
    ) // Branch on EQual
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x05, bne,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .readsRs=true,
        .readsRt=true
    ) // Branch on Not Equal
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x14, beql,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true,
        .readsRt=true
    ) // Branch on EQual Likely
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x15, bnel,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true,
        .readsRt=true
    ) // Branch on Not Equal Likely

    // OP rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x06, blez,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .readsRs=true
    ) // Branch on Less than or Equal to Zero
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x16, blezl,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true
    ) // Branch on Less than or Equal to Zero Likely
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x07, bgtz,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .readsRs=true
    ) // Branch on Greater Than Zero
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x17, bgtzl,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true
    ) // Branch on Greater Than Zero Likely

    // OP rt, rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x08, addi,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .notEmittedByCompilers=true,
        .canBeLo=true
    ) // Add Immediate
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x09, addiu,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true
    ) // Add Immediate Unsigned Word
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0A, slti,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true
    ) // Set on Less Than Immediate
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0B, sltiu,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true
    ) // Set on Less Than Immediate Unsigned
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0C, andi,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .readsRs=true
    ) // And Immediate
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0D, ori,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true
    ) // Or Immediate
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0E, xori,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .readsRs=true
    ) // eXclusive OR Immediate
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x18, daddi,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true
    ) // Doubleword add Immediate
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x19, daddiu,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true
    ) // Doubleword add Immediate Unsigned

    // OP rt, IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0F, lui,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isUnsigned=true,
        .modifiesRt=true,
        .canBeHi=true
    ) // Load Upper Immediate

    // OP rt, IMM(base)
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x1A, ldl,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Doubleword Left
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x1B, ldr,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Doubleword Right
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x20, lb,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_BYTE
    ) // Load Byte
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x21, lh,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_SHORT
    ) // Load Halfword
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x22, lwl,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Word Left
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x23, lw,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_WORD
    ) // Load Word
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x24, lbu,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
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
        cpu, 0x25, lhu,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
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
        cpu, 0x26, lwr,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Word Right
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x27, lwu,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_WORD,
        .doesUnsignedMemoryAccess=true
    ) // Load Word Unsigned
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x28, sb,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_BYTE
    ) // Store Byte
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x29, sh,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_SHORT
    ) // Store Halfword
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x2A, swl,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Word Left
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x2B, sw,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_WORD
    ) // Store Word
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x2C, sdl,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Doubleword Left
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x2D, sdr,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Doubleword Right
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x2E, swr,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Word Right
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x30, ll,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .notEmittedByCompilers=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Linked word
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x33, pref,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true
    ) // Prefetch
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x34, lld,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .notEmittedByCompilers=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Linked Doubleword
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x37, ld,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .modifiesRt=true,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_DOUBLEWORD
    ) // Load Doubleword
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x38, sc,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .notEmittedByCompilers=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Conditional word
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x3C, scd,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .notEmittedByCompilers=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Conditional Doubleword
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x3F, sd,
        .operands={RAB_OPERAND_cpu_rt, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .readsRt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_DOUBLEWORD
    ) // Store Doubleword

    // OP op, IMM(base)
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x2F, cache,
        .operands={RAB_OPERAND_cpu_op, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .notEmittedByCompilers=true
    ) // Cache

    // OP ft, IMM(base)
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x31, lwc1,
        .operands={RAB_OPERAND_cpu_ft, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isFloat=true,
        .readsRs=true,
        .modifiesFt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_FLOAT
    ) // Load Word to Coprocessor z
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x35, ldc1,
        .operands={RAB_OPERAND_cpu_ft, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isFloat=true,
        .isDouble=true,
        .readsRs=true,
        .modifiesFt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true,
        .accessType=RAB_ACCESSTYPE_DOUBLEFLOAT
    ) // Load Doubleword to Coprocessor z
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x39, swc1,
        .operands={RAB_OPERAND_cpu_ft, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isFloat=true,
        .readsRs=true,
        .readsFt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_FLOAT
    ) // Store Word from Coprocessor z
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x3D, sdc1,
        .operands={RAB_OPERAND_cpu_ft, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isFloat=true,
        .isDouble=true,
        .readsRs=true,
        .readsFt=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true,
        .accessType=RAB_ACCESSTYPE_DOUBLEFLOAT
    ) // Store Doubleword from Coprocessor z

    // OP cop2t, IMM(base)
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x32, lwc2,
        .operands={RAB_OPERAND_cpu_cop2t, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Word to Coprocessor z
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x36, ldc2,
        .operands={RAB_OPERAND_cpu_cop2t, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesLoad=true
    ) // Load Doubleword to Coprocessor z
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x3A, swc2,
        .operands={RAB_OPERAND_cpu_cop2t, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Word from Coprocessor z
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x3E, sdc2,
        .operands={RAB_OPERAND_cpu_cop2t, RAB_OPERAND_cpu_immediate_base},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .canBeLo=true,
        .doesDereference=true,
        .doesStore=true
    ) // Store Doubleword from Coprocessor z


    // Pseudo-Instruction Unique IDs

    // OP IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, -0x03, b,
        .operands={RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .isBranch=true,
        .isPseudo=true
    ) // Branch (unconditional)

    // OP rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, -0x04, beqz,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .isBranch=true,
        .isPseudo=true
    ) // Branch on EQual Zero
    RABBITIZER_DEF_INSTR_ID(
        cpu, -0x05, bnez,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_I,
        .readsRs=true,
        .isBranch=true,
        .isPseudo=true
    ) // Branch on Not Equal Zero
