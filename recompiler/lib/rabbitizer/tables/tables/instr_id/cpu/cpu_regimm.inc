/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

    // OP rs, IMM
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x00, bltz,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .readsRs=true
    ) // Branch on Less Than Zero
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x01, bgez,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .readsRs=true
    ) // Branch on Greater than or Equal to Zero
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x02, bltzl,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true
    ) // Branch on Less Than Zero Likely
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x03, bgezl,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true
    ) // Branch on Greater than or Equal to Zero Likely

    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x08, tgei,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isTrap=true,
        .readsRs=true,
        .notEmittedByCompilers=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x09, tgeiu,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isTrap=true,
        .readsRs=true,
        .notEmittedByCompilers=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0A, tlti,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isTrap=true,
        .readsRs=true,
        .notEmittedByCompilers=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0B, tltiu,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isTrap=true,
        .readsRs=true,
        .notEmittedByCompilers=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0C, teqi,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isTrap=true,
        .readsRs=true,
        .notEmittedByCompilers=true
    )
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x0E, tnei,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_immediate},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isTrap=true,
        .readsRs=true,
        .notEmittedByCompilers=true
    )

    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x10, bltzal,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .readsRs=true,
        .doesLink=true
    ) // Branch on Less Than Zero and Link
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x11, bgezal,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .readsRs=true,
        .doesLink=true
    ) // Branch on Greater Than or Equal to Zero and Link
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x12, bltzall,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true,
        .doesLink=true
    ) // Branch on Less Than Zero and Link Likely
    RABBITIZER_DEF_INSTR_ID(
        cpu, 0x13, bgezall,
        .operands={RAB_OPERAND_cpu_rs, RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .isBranchLikely=true,
        .readsRs=true,
        .notEmittedByCompilers=true,
        .doesLink=true
    ) // Branch on Greater Than or Equal to Zero and Link Likely


    // Pseudo-Instruction Unique IDs

    RABBITIZER_DEF_INSTR_ID(
        cpu, -0x11, bal,
        .operands={RAB_OPERAND_cpu_branch_target_label},
        .instrType=RABBITIZER_INSTR_TYPE_REGIMM,
        .isBranch=true,
        .notEmittedByCompilers=true,
        .doesLink=true,
        .isPseudo=true
    ) // Branch and Link
