/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#include "instructions/RabbitizerInstruction.h"

#include <assert.h>

#include "common/RabbitizerConfig.h"
#include "instructions/RabbitizerInstructionRsp.h"
#include "instructions/RabbitizerInstructionR3000GTE.h"
#include "instructions/RabbitizerInstructionR5900.h"
#include "instructions/RabbitizerRegister.h"

// TODO: deprecate
bool RabbitizerInstruction_isImplemented(const RabbitizerInstruction *self) {
    return RabbitizerInstrId_isValid(self->uniqueId);
}

bool RabbitizerInstruction_isLikelyHandwritten(const RabbitizerInstruction *self) {
    if (self->_handwrittenCategory) {
        return true;
    }

    if (RabbitizerInstrDescriptor_isIType(self->descriptor) && !RabbitizerInstrDescriptor_isFloat(self->descriptor)) {
        if (RAB_INSTR_GET_rs(self) == RABBITIZER_REG_GPR_O32_k0 ||
            RAB_INSTR_GET_rs(self) == RABBITIZER_REG_GPR_O32_k1) {
            return true;
        }
        if (RAB_INSTR_GET_rt(self) == RABBITIZER_REG_GPR_O32_k0 ||
            RAB_INSTR_GET_rt(self) == RABBITIZER_REG_GPR_O32_k1) {
            return true;
        }
    }

    if (RabbitizerInstrDescriptor_notEmittedByCompilers(self->descriptor)) {
        return true;
    }

    return false;
}

bool RabbitizerInstruction_isNop(const RabbitizerInstruction *self) {
    return self->word == 0;
}

bool RabbitizerInstruction_isUnconditionalBranch(const RabbitizerInstruction *self) {
    switch (self->uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_b:
        case RABBITIZER_INSTR_ID_rsp_b:
            return true;

        case RABBITIZER_INSTR_ID_cpu_beq:
        case RABBITIZER_INSTR_ID_rsp_beq:
            // in case the b pseudoinstruction is disabled
            return RAB_INSTR_GET_rt(self) == 0 && RAB_INSTR_GET_rs(self) == 0;

        case RABBITIZER_INSTR_ID_cpu_j:
        case RABBITIZER_INSTR_ID_rsp_j:
            return RabbitizerConfig_Cfg.toolchainTweaks.treatJAsUnconditionalBranch;

        default:
            return false;
    }
}

bool RabbitizerInstruction_isReturn(const RabbitizerInstruction *self) {
    switch (self->uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_jr:
        case RABBITIZER_INSTR_ID_rsp_jr: {
            const RabbitizerRegisterDescriptor *regDescriptor =
                RabbitizerRegister_getDescriptor_Gpr(RAB_INSTR_GET_rs(self));

            return RabbitizerRegisterDescriptor_isRa(regDescriptor);
        }

        default:
            return false;
    }
}

bool RabbitizerInstruction_isJumptableJump(const RabbitizerInstruction *self) {
    switch (self->uniqueId) {
        case RABBITIZER_INSTR_ID_cpu_jr:
        case RABBITIZER_INSTR_ID_rsp_jr: {
            const RabbitizerRegisterDescriptor *regDescriptor =
                RabbitizerRegister_getDescriptor_Gpr(RAB_INSTR_GET_rs(self));

            return !RabbitizerRegisterDescriptor_isRa(regDescriptor);
        }

        default:
            return false;
    }
}

bool RabbitizerInstruction_hasDelaySlot(const RabbitizerInstruction *self) {
    return RabbitizerInstrDescriptor_isBranch(self->descriptor) || RabbitizerInstrDescriptor_isJump(self->descriptor);
}

const char *RabbitizerInstruction_mapInstrToType(const RabbitizerInstruction *self) {
    if (RabbitizerInstrDescriptor_isDouble(self->descriptor)) {
        return "f64";
    }
    if (RabbitizerInstrDescriptor_isFloat(self->descriptor)) {
        return "f32";
    }
    if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_lwu) {
        return "u32";
    }
    if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_lh || self->uniqueId == RABBITIZER_INSTR_ID_cpu_sh) {
        return "s16";
    }
    if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_lhu) {
        return "u16";
    }
    if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_lb || self->uniqueId == RABBITIZER_INSTR_ID_cpu_sb) {
        return "s8";
    }
    if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_lbu) {
        return "u8";
    }
    if (self->uniqueId == RABBITIZER_INSTR_ID_cpu_ld || self->uniqueId == RABBITIZER_INSTR_ID_cpu_sd) {
        return "s64";
    }
    return NULL;
}

bool RabbitizerInstruction_sameOpcode(const RabbitizerInstruction *self, const RabbitizerInstruction *other) {
    if (!RabbitizerInstrId_isValid(self->uniqueId) || !RabbitizerInstrId_isValid(other->uniqueId)) {
        return false;
    }
    return self->uniqueId == other->uniqueId;
}

bool RabbitizerInstruction_sameOpcodeButDifferentArguments(const RabbitizerInstruction *self,
                                                           const RabbitizerInstruction *other) {
    if (!RabbitizerInstruction_sameOpcode(self, other)) {
        return false;
    }
    return RabbitizerInstruction_getRaw(self) != RabbitizerInstruction_getRaw(other);
}

// TODO: deprecate?
bool RabbitizerInstruction_hasOperand(const RabbitizerInstruction *self, RabbitizerOperandType operand) {
    return RabbitizerInstrDescriptor_hasSpecificOperand(self->descriptor, operand);
}

bool RabbitizerInstruction_hasOperandAlias(const RabbitizerInstruction *self, RabbitizerOperandType operand) {
    return RabbitizerInstrDescriptor_hasOperandAlias(self->descriptor, operand);
}

uint32_t RabbitizerInstruction_getValidBits(const RabbitizerInstruction *self) {
    size_t i;
    uint32_t validbits;

    validbits = self->_mandatorybits;

    for (i = 0; i < ARRAY_COUNT(self->descriptor->operands) && self->descriptor->operands[i] != RAB_OPERAND_ALL_INVALID;
         i++) {

        switch (self->descriptor->operands[i]) {
            case RAB_OPERAND_cpu_rs:
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_rt:
                validbits = RAB_INSTR_PACK_rt(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_rd:
                validbits = RAB_INSTR_PACK_rd(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_sa:
                validbits = RAB_INSTR_PACK_sa(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_zero:
                break;

            case RAB_OPERAND_cpu_cop0d:
                validbits = RAB_INSTR_PACK_cop0d(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_fs:
                validbits = RAB_INSTR_PACK_fs(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_ft:
                validbits = RAB_INSTR_PACK_ft(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_fd:
                validbits = RAB_INSTR_PACK_fd(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_cop1cs:
                validbits = RAB_INSTR_PACK_cop1cs(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_cop2t:
                validbits = RAB_INSTR_PACK_cop2t(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_cop2cd:
                validbits = RAB_INSTR_PACK_cop2cd(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_op:
                validbits = RAB_INSTR_PACK_op(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_code:
                validbits = RAB_INSTR_PACK_code(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_code_lower:
                validbits = RAB_INSTR_PACK_code_lower(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_copraw:
                validbits = RAB_INSTR_PACK_copraw(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_label:
                validbits = RAB_INSTR_PACK_instr_index(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_immediate:
                validbits = RAB_INSTR_PACK_immediate(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_branch_target_label:
                validbits = RAB_INSTR_PACK_immediate(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_immediate_base:
                validbits = RAB_INSTR_PACK_immediate(validbits, ~0);
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;

            case RAB_OPERAND_cpu_maybe_rd_rs:
                validbits = RAB_INSTR_PACK_rd(validbits, ~0);
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;

            /* rsp */
            case RAB_OPERAND_rsp_rs:
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_rt:
                validbits = RAB_INSTR_PACK_rt(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_rd:
                validbits = RAB_INSTR_PACK_rd(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_cop0d:
                validbits = RAB_INSTR_PACK_cop0d(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_cop2t:
                validbits = RAB_INSTR_RSP_PACK_cop2t(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_cop2cd:
                validbits = RAB_INSTR_RSP_PACK_cop2cd(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vs:
                validbits = RAB_INSTR_RSP_PACK_vs(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vt:
                validbits = RAB_INSTR_RSP_PACK_vt(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vd:
                validbits = RAB_INSTR_RSP_PACK_vd(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vt_elementhigh:
                validbits = RAB_INSTR_RSP_PACK_vt(validbits, ~0);
                validbits = RAB_INSTR_RSP_PACK_elementhigh(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vt_elementlow:
                validbits = RAB_INSTR_RSP_PACK_vt(validbits, ~0);
                validbits = RAB_INSTR_RSP_PACK_elementlow(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vd_de:
                validbits = RAB_INSTR_RSP_PACK_vd(validbits, ~0);
                validbits = RAB_INSTR_RSP_PACK_de(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_vs_index:
                validbits = RAB_INSTR_RSP_PACK_vs(validbits, ~0);
                validbits = RAB_INSTR_RSP_PACK_index(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_offset_rs:
                validbits = RAB_INSTR_RSP_PACK_offset(validbits, ~0);
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_immediate_base:
                validbits = RAB_INSTR_PACK_immediate(validbits, ~0);
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;

            case RAB_OPERAND_rsp_maybe_rd_rs:
                validbits = RAB_INSTR_PACK_rd(validbits, ~0);
                validbits = RAB_INSTR_PACK_rs(validbits, ~0);
                break;
            /* rsp */

            /* r3000gte */
            case RAB_OPERAND_r3000gte_sf:
                validbits = RAB_INSTR_R3000GTE_PACK_sf(validbits, ~0);
                break;

            case RAB_OPERAND_r3000gte_mx:
                validbits = RAB_INSTR_R3000GTE_PACK_mx(validbits, ~0);
                break;

            case RAB_OPERAND_r3000gte_v:
                validbits = RAB_INSTR_R3000GTE_PACK_v(validbits, ~0);
                break;

            case RAB_OPERAND_r3000gte_cv:
                validbits = RAB_INSTR_R3000GTE_PACK_cv(validbits, ~0);
                break;

            case RAB_OPERAND_r3000gte_lm:
                validbits = RAB_INSTR_R3000GTE_PACK_lm(validbits, ~0);
                break;
            /* r3000gte */

            /* r5900 */
            case RAB_OPERAND_r5900_I:
            case RAB_OPERAND_r5900_Q:
            case RAB_OPERAND_r5900_R:
            case RAB_OPERAND_r5900_ACC:
                // Not real registers encoded on the instruction itself
                break;

            case RAB_OPERAND_r5900_ACCxyzw:
                validbits = RAB_INSTR_R5900_PACK_xyzw_x(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_y(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_z(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_w(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfs:
                validbits = RAB_INSTR_R5900_PACK_vfs(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vft:
                validbits = RAB_INSTR_R5900_PACK_vft(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfd:
                validbits = RAB_INSTR_R5900_PACK_vfd(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfsxyzw:
                validbits = RAB_INSTR_R5900_PACK_vfs(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_x(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_y(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_z(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_w(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vftxyzw:
                validbits = RAB_INSTR_R5900_PACK_vft(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_x(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_y(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_z(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_w(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfdxyzw:
                validbits = RAB_INSTR_R5900_PACK_vfd(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_x(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_y(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_z(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_xyzw_w(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfsn:
                validbits = RAB_INSTR_R5900_PACK_vfs(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_n(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vftn:
                validbits = RAB_INSTR_R5900_PACK_vft(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_n(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfdn:
                validbits = RAB_INSTR_R5900_PACK_vfd(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_n(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfsl:
                validbits = RAB_INSTR_R5900_PACK_vfs(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_l(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vftl:
                validbits = RAB_INSTR_R5900_PACK_vft(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_l(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfdl:
                validbits = RAB_INSTR_R5900_PACK_vfd(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_l(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfsm:
                validbits = RAB_INSTR_R5900_PACK_vfs(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_m(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vftm:
                validbits = RAB_INSTR_R5900_PACK_vft(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_m(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vfdm:
                validbits = RAB_INSTR_R5900_PACK_vfd(validbits, ~0);
                validbits = RAB_INSTR_R5900_PACK_m(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vis:
                validbits = RAB_INSTR_R5900_PACK_vis(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vit:
                validbits = RAB_INSTR_R5900_PACK_vit(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vid:
                validbits = RAB_INSTR_R5900_PACK_vid(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vis_predecr:
                validbits = RAB_INSTR_R5900_PACK_vis(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vit_predecr:
                validbits = RAB_INSTR_R5900_PACK_vit(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vid_predecr:
                validbits = RAB_INSTR_R5900_PACK_vid(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vis_postincr:
                validbits = RAB_INSTR_R5900_PACK_vis(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vit_postincr:
                validbits = RAB_INSTR_R5900_PACK_vit(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_vid_postincr:
                validbits = RAB_INSTR_R5900_PACK_vid(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_immediate5:
                validbits = RAB_INSTR_R5900_PACK_imm5(validbits, ~0);
                break;

            case RAB_OPERAND_r5900_immediate15:
                validbits = RAB_INSTR_R5900_PACK_imm15(validbits, ~0);
                break;
                /* r5900 */

            case RAB_OPERAND_ALL_INVALID:
            case RAB_OPERAND_ALL_MAX:
                assert(self->descriptor->operands[i] != RAB_OPERAND_ALL_INVALID &&
                       self->descriptor->operands[i] != RAB_OPERAND_ALL_MAX);
                break;
        }
    }

    return validbits;
}

bool RabbitizerInstruction_isValid(const RabbitizerInstruction *self) {
    uint32_t validbits;

    if (!RabbitizerInstrId_isValid(self->uniqueId)) {
        return false;
    }

    validbits = RabbitizerInstruction_getValidBits(self);

    return ((~validbits) & self->word) == 0;
}
