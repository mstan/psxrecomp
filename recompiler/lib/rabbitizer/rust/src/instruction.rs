/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

use crate::{
    access_type_enum, instr_category_enum, instr_descriptor, instr_id_enum, instr_id_type_enum,
    instr_suffix_enum, operand_type_enum, registers_enum, utils,
};

#[repr(C)]
#[derive(Debug, Clone)]
#[allow(non_camel_case_types)]
pub struct Instruction {
    word: u32,
    _mandatorybits: u32,
    pub unique_id: instr_id_enum::InstrId,
    descriptor: *const instr_descriptor::InstrDescriptor,
    instr_id_type: instr_id_type_enum::InstrIdType,
    pub vram: u32,
    _handwritten_category: bool,
    pub in_handwritten_function: bool,
    pub category: instr_category_enum::InstrCategory,
}

#[link(name = "rabbitizer", kind = "static")]
extern "C" {
    fn RabbitizerInstrId_getOpcodeName(
        unique_id: instr_id_enum::InstrId,
    ) -> *const core::ffi::c_char;
}

extern "C" {
    fn RabInstrIdType_getName(id_type: instr_id_type_enum::InstrIdType)
        -> *const core::ffi::c_char;
}

extern "C" {
    fn RabbitizerInstruction_init(self_: *mut Instruction, word: u32, vram: u32);
    fn RabbitizerInstruction_destroy(self_: *mut Instruction);

    fn RabbitizerInstruction_processUniqueId(self_: *mut Instruction);
}

extern "C" {
    fn RabbitizerInstructionRsp_init(self_: *mut Instruction, word: u32, vram: u32);
    fn RabbitizerInstructionRsp_destroy(self_: *mut Instruction);

    fn RabbitizerInstructionRsp_processUniqueId(self_: *mut Instruction);
}

extern "C" {
    fn RabbitizerInstructionR3000GTE_init(self_: *mut Instruction, word: u32, vram: u32);
    fn RabbitizerInstructionR3000GTE_destroy(self_: *mut Instruction);

    fn RabbitizerInstructionR3000GTE_processUniqueId(self_: *mut Instruction);
}

extern "C" {
    fn RabbitizerInstructionR5900_init(self_: *mut Instruction, word: u32, vram: u32);
    fn RabbitizerInstructionR5900_destroy(self_: *mut Instruction);

    fn RabbitizerInstructionR5900_processUniqueId(self_: *mut Instruction);
}

extern "C" {
    fn RabbitizerInstruction_getRaw(self_: *const Instruction) -> u32;
    fn RabbitizerInstruction_getProcessedImmediate(self_: *const Instruction) -> i32;
    fn RabbitizerInstruction_getInstrIndexAsVram(self_: *const Instruction) -> u32;
    fn RabbitizerInstruction_getBranchOffset(self_: *const Instruction) -> i32;

    fn RabbitizerInstruction_getBranchOffsetGeneric(self_: *const Instruction) -> i32;
    fn RabbitizerInstruction_getBranchVramGeneric(self_: *const Instruction) -> i32;
    fn RabbitizerInstruction_getDestinationGpr(self_: *const Instruction) -> i8;
    fn RabbitizerInstruction_outputsToGprZero(self_: *const Instruction) -> bool;
    fn RabbitizerInstruction_blankOut(self_: *mut Instruction);
    fn RabbitizerInstruction_isImplemented(self_: *const Instruction) -> bool;
    fn RabbitizerInstruction_isLikelyHandwritten(self_: *const Instruction) -> bool;
    fn RabbitizerInstruction_isNop(self_: *const Instruction) -> bool;
    fn RabbitizerInstruction_isUnconditionalBranch(self_: *const Instruction) -> bool;
    fn RabbitizerInstruction_isReturn(self_: *const Instruction) -> bool;
    fn RabbitizerInstruction_isJumptableJump(self_: *const Instruction) -> bool;

    fn RabbitizerInstruction_hasDelaySlot(self_: *const Instruction) -> bool;

    fn RabbitizerInstruction_sameOpcode(
        self_: *const Instruction,
        other: *const Instruction,
    ) -> bool;
    fn RabbitizerInstruction_sameOpcodeButDifferentArguments(
        self_: *const Instruction,
        other: *const Instruction,
    ) -> bool;
    fn RabbitizerInstruction_hasOperand(
        self_: *const Instruction,
        operand: operand_type_enum::OperandType,
    ) -> bool;
    fn RabbitizerInstruction_hasOperandAlias(
        self_: *const Instruction,
        operand: operand_type_enum::OperandType,
    ) -> bool;

    fn RabbitizerInstruction_isValid(self_: *const Instruction) -> bool;

    fn RabbitizerInstruction_getSizeForBuffer(
        self_: *const Instruction,
        immOverrideLength: utils::SizeT,
        extraLJust: core::ffi::c_int,
    ) -> utils::SizeT;
    fn RabbitizerInstruction_disassemble(
        self_: *const Instruction,
        dst: *mut core::ffi::c_char,
        immOverride: *const core::ffi::c_char,
        immOverrideLength: utils::SizeT,
        extraLJust: core::ffi::c_int,
    ) -> utils::SizeT;
}

extern "C" {
    fn RabbitizerInstrDescriptor_instrSuffix(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> instr_suffix_enum::InstrSuffix;
    fn RabbitizerInstrDescriptor_isBranch(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_isBranchLikely(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_isJump(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_isJumpWithAddress(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_isTrap(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_isFloat(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_isDouble(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_isUnsigned(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;

    fn RabbitizerInstrDescriptor_modifiesRs(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_modifiesRt(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_modifiesRd(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_readsRs(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_readsRt(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_readsRd(self_: *const instr_descriptor::InstrDescriptor) -> bool;

    fn RabbitizerInstrDescriptor_readsHI(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_readsLO(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_modifiesHI(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_modifiesLO(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;

    fn RabbitizerInstrDescriptor_modifiesFs(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_modifiesFt(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_modifiesFd(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_readsFs(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_readsFt(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_readsFd(self_: *const instr_descriptor::InstrDescriptor) -> bool;

    fn RabbitizerInstrDescriptor_notEmittedByCompilers(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_canBeHi(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_canBeLo(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_doesLink(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_doesDereference(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_doesLoad(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_doesStore(self_: *const instr_descriptor::InstrDescriptor)
        -> bool;
    fn RabbitizerInstrDescriptor_maybeIsMove(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
    fn RabbitizerInstrDescriptor_isPseudo(self_: *const instr_descriptor::InstrDescriptor) -> bool;
    fn RabbitizerInstrDescriptor_getAccessType(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> access_type_enum::AccessType;
    fn RabbitizerInstrDescriptor_doesUnsignedMemoryAccess(
        self_: *const instr_descriptor::InstrDescriptor,
    ) -> bool;
}

impl Drop for Instruction {
    fn drop(&mut self) {
        unsafe {
            match self.category {
                instr_category_enum::InstrCategory::CPU => {
                    RabbitizerInstruction_destroy(self);
                }
                instr_category_enum::InstrCategory::RSP => {
                    RabbitizerInstructionRsp_destroy(self);
                }
                instr_category_enum::InstrCategory::R3000GTE => {
                    RabbitizerInstructionR3000GTE_destroy(self);
                }
                instr_category_enum::InstrCategory::R5900 => {
                    RabbitizerInstructionR5900_destroy(self);
                }
                instr_category_enum::InstrCategory::MAX => {
                    core::panic!();
                }
            }
        }
    }
}

impl Instruction {
    pub fn new(word: u32, vram: u32, instr_cat: instr_category_enum::InstrCategory) -> Self {
        unsafe {
            let mut instr: std::mem::MaybeUninit<Instruction> = std::mem::MaybeUninit::uninit();
            match instr_cat {
                instr_category_enum::InstrCategory::CPU => {
                    RabbitizerInstruction_init(instr.as_mut_ptr(), word, vram);
                    RabbitizerInstruction_processUniqueId(instr.as_mut_ptr());
                }
                instr_category_enum::InstrCategory::RSP => {
                    RabbitizerInstructionRsp_init(instr.as_mut_ptr(), word, vram);
                    RabbitizerInstructionRsp_processUniqueId(instr.as_mut_ptr());
                }
                instr_category_enum::InstrCategory::R3000GTE => {
                    RabbitizerInstructionR3000GTE_init(instr.as_mut_ptr(), word, vram);
                    RabbitizerInstructionR3000GTE_processUniqueId(instr.as_mut_ptr());
                }
                instr_category_enum::InstrCategory::R5900 => {
                    RabbitizerInstructionR5900_init(instr.as_mut_ptr(), word, vram);
                    RabbitizerInstructionR5900_processUniqueId(instr.as_mut_ptr());
                }
                instr_category_enum::InstrCategory::MAX => {
                    core::panic!();
                } // _ => not used in purpose
            }
            instr.assume_init()
        }
    }

    pub fn get_opcode(&self) -> u32 {
        utils::shiftr(self.word, 26, 6)
    }

    pub fn get_rs(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_rs) {
            core::panic!();
        }

        utils::shiftr(self.word, 21, 5)
    }

    pub fn get_rs_o32(&self) -> registers_enum::registers::GprO32 {
        self.get_rs().try_into().unwrap()
    }

    pub fn get_rs_n32(&self) -> registers_enum::registers::GprN32 {
        self.get_rs().try_into().unwrap()
    }

    pub fn get_rt(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_rt) {
            core::panic!();
        }

        utils::shiftr(self.word, 16, 5)
    }

    pub fn get_rt_o32(&self) -> registers_enum::registers::GprO32 {
        self.get_rt().try_into().unwrap()
    }

    pub fn get_rt_n32(&self) -> registers_enum::registers::GprN32 {
        self.get_rt().try_into().unwrap()
    }

    pub fn get_rd(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_rd) {
            core::panic!();
        }

        utils::shiftr(self.word, 11, 5)
    }

    pub fn get_rd_o32(&self) -> registers_enum::registers::GprO32 {
        self.get_rd().try_into().unwrap()
    }

    pub fn get_rd_n32(&self) -> registers_enum::registers::GprN32 {
        self.get_rd().try_into().unwrap()
    }

    pub fn get_sa(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_sa) {
            core::panic!();
        }

        utils::shiftr(self.word, 6, 5)
    }

    pub fn get_function(&self) -> u32 {
        //if !self.has_operand_alias(operand_type_enum::OperandType::cpu_function) {
        //    core::panic!();
        //}

        utils::shiftr(self.word, 0, 6)
    }

    pub fn get_cop0d(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_cop0d) {
            core::panic!();
        }

        utils::shiftr(self.word, 11, 5)
    }

    pub fn get_cop0d_cop0(&self) -> registers_enum::registers::Cop0 {
        self.get_cop0d().try_into().unwrap()
    }

    pub fn get_instr_index(&self) -> u32 {
        utils::shiftr(self.word, 0, 26)
    }

    pub fn get_immediate(&self) -> u16 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_immediate) {
            core::panic!();
        }

        utils::shiftr(self.word, 0, 16).try_into().unwrap()
    }

    pub fn get_code(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_code) {
            core::panic!();
        }

        utils::shiftr(self.word, 6, 20)
    }

    pub fn get_code_upper(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_code) {
            core::panic!();
        }

        utils::shiftr(self.word, 16, 10)
    }

    pub fn get_code_lower(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_code_lower) {
            core::panic!();
        }

        utils::shiftr(self.word, 6, 10)
    }

    pub fn get_copraw(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_copraw) {
            core::panic!();
        }

        utils::shiftr(self.word, 0, 25)
    }

    pub fn get_fs(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_fs) {
            core::panic!();
        }

        utils::shiftr(self.word, 11, 5)
    }

    pub fn get_fs_o32(&self) -> registers_enum::registers::Cop1O32 {
        self.get_fs().try_into().unwrap()
    }

    pub fn get_fs_n32(&self) -> registers_enum::registers::Cop1N32 {
        self.get_fs().try_into().unwrap()
    }

    pub fn get_fs_n64(&self) -> registers_enum::registers::Cop1N64 {
        self.get_fs().try_into().unwrap()
    }

    pub fn get_ft(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_ft) {
            core::panic!();
        }

        utils::shiftr(self.word, 16, 5)
    }

    pub fn get_ft_o32(&self) -> registers_enum::registers::Cop1O32 {
        self.get_ft().try_into().unwrap()
    }

    pub fn get_ft_n32(&self) -> registers_enum::registers::Cop1N32 {
        self.get_ft().try_into().unwrap()
    }

    pub fn get_ft_n64(&self) -> registers_enum::registers::Cop1N64 {
        self.get_ft().try_into().unwrap()
    }

    pub fn get_fd(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_fd) {
            core::panic!();
        }

        utils::shiftr(self.word, 6, 5)
    }

    pub fn get_fd_o32(&self) -> registers_enum::registers::Cop1O32 {
        self.get_fd().try_into().unwrap()
    }

    pub fn get_fd_n32(&self) -> registers_enum::registers::Cop1N32 {
        self.get_fd().try_into().unwrap()
    }

    pub fn get_fd_n64(&self) -> registers_enum::registers::Cop1N64 {
        self.get_fd().try_into().unwrap()
    }

    pub fn get_cop1cs(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_cop1cs) {
            core::panic!();
        }

        utils::shiftr(self.word, 11, 5)
    }

    pub fn get_cop1cs_cop1control(&self) -> registers_enum::registers::Cop1Control {
        self.get_cop1cs().try_into().unwrap()
    }

    pub fn get_cop2t(&self) -> u32 {
        if !self.has_operand_alias(operand_type_enum::OperandType::cpu_cop2t) {
            core::panic!();
        }

        utils::shiftr(self.word, 16, 5)
    }

    pub fn get_cop2t_cop2(&self) -> registers_enum::registers::Cop2 {
        self.get_cop2t().try_into().unwrap()
    }

    pub fn instr_id_type_name(&self) -> &'static str {
        unsafe { std::ffi::CStr::from_ptr(RabInstrIdType_getName(self.instr_id_type)) }
            .to_str()
            .unwrap()
    }

    pub fn raw(&self) -> u32 {
        unsafe { RabbitizerInstruction_getRaw(self) }
    }
    pub fn processed_immediate(&self) -> i32 {
        unsafe { RabbitizerInstruction_getProcessedImmediate(self) }
    }
    pub fn instr_index_as_vram(&self) -> u32 {
        unsafe { RabbitizerInstruction_getInstrIndexAsVram(self) }
    }
    pub fn branch_offset(&self) -> i32 {
        unsafe { RabbitizerInstruction_getBranchOffset(self) }
    }

    pub fn branch_offset_generic(&self) -> i32 {
        unsafe { RabbitizerInstruction_getBranchOffsetGeneric(self) }
    }

    pub fn branch_vram_generic(&self) -> i32 {
        unsafe { RabbitizerInstruction_getBranchVramGeneric(self) }
    }
    pub fn destination_gpr(&self) -> Option<u32> {
        unsafe {
            let reg: i8 = RabbitizerInstruction_getDestinationGpr(self);

            if reg < 0 {
                return None;
            }
            Some(reg as u32)
        }
    }
    pub fn outputs_to_gpr_zero(&self) -> bool {
        unsafe { RabbitizerInstruction_outputsToGprZero(self) }
    }
    pub fn opcode_name(&self) -> &'static str {
        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerInstrId_getOpcodeName(self.unique_id))
                .to_str()
                .unwrap()
        }
    }

    pub fn blank_out(mut self) {
        unsafe { RabbitizerInstruction_blankOut(&mut self) }
    }

    pub fn is_implemented(&self) -> bool {
        unsafe { RabbitizerInstruction_isImplemented(self) }
    }
    pub fn is_likely_handwritten(&self) -> bool {
        unsafe { RabbitizerInstruction_isLikelyHandwritten(self) }
    }
    pub fn is_nop(&self) -> bool {
        unsafe { RabbitizerInstruction_isNop(self) }
    }
    pub fn is_unconditional_branch(&self) -> bool {
        unsafe { RabbitizerInstruction_isUnconditionalBranch(self) }
    }

    pub fn is_return(&self) -> bool {
        unsafe { RabbitizerInstruction_isReturn(self) }
    }
    pub fn is_jumptable_jump(&self) -> bool {
        unsafe { RabbitizerInstruction_isJumptableJump(self) }
    }

    pub fn has_delay_slot(&self) -> bool {
        unsafe { RabbitizerInstruction_hasDelaySlot(self) }
    }

    pub fn same_opcode(&self, other: &Instruction) -> bool {
        unsafe { RabbitizerInstruction_sameOpcode(self, other) }
    }
    pub fn same_opcode_but_different_arguments(&self, other: &Instruction) -> bool {
        unsafe { RabbitizerInstruction_sameOpcodeButDifferentArguments(self, other) }
    }

    pub fn has_operand(&self, operand: operand_type_enum::OperandType) -> bool {
        unsafe { RabbitizerInstruction_hasOperand(self, operand) }
    }
    pub fn has_operand_alias(&self, operand: operand_type_enum::OperandType) -> bool {
        unsafe { RabbitizerInstruction_hasOperandAlias(self, operand) }
    }

    pub fn is_valid(&self) -> bool {
        unsafe { RabbitizerInstruction_isValid(self) }
    }

    pub fn get_operand_type(&self, index: usize) -> operand_type_enum::OperandType {
        unsafe { self.descriptor.as_ref().unwrap().get_operand_type(index) }
    }

    pub fn get_operands_slice(&self) -> &[operand_type_enum::OperandType] {
        unsafe { self.descriptor.as_ref().unwrap().operands_slice() }
    }

    pub fn instr_suffix(&self) -> instr_suffix_enum::InstrSuffix {
        unsafe { RabbitizerInstrDescriptor_instrSuffix(self.descriptor) }
    }
    pub fn is_branch(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isBranch(self.descriptor) }
    }
    pub fn is_branch_likely(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isBranchLikely(self.descriptor) }
    }
    pub fn is_jump(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isJump(self.descriptor) }
    }
    pub fn is_jump_with_address(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isJumpWithAddress(self.descriptor) }
    }
    pub fn is_trap(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isTrap(self.descriptor) }
    }
    pub fn is_float(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isFloat(self.descriptor) }
    }
    pub fn is_double(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isDouble(self.descriptor) }
    }
    pub fn is_unsigned(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isUnsigned(self.descriptor) }
    }
    pub fn modifies_rs(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesRs(self.descriptor) }
    }
    pub fn modifies_rt(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesRt(self.descriptor) }
    }
    pub fn modifies_rd(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesRd(self.descriptor) }
    }
    pub fn reads_rs(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsRs(self.descriptor) }
    }
    pub fn reads_rt(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsRt(self.descriptor) }
    }
    pub fn reads_rd(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsRd(self.descriptor) }
    }
    pub fn reads_hi(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsHI(self.descriptor) }
    }
    pub fn reads_lo(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsLO(self.descriptor) }
    }
    pub fn modifies_hi(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesHI(self.descriptor) }
    }
    pub fn modifies_lo(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesLO(self.descriptor) }
    }
    pub fn modifies_fs(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesFs(self.descriptor) }
    }
    pub fn modifies_ft(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesFt(self.descriptor) }
    }
    pub fn modifies_fd(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_modifiesFd(self.descriptor) }
    }
    pub fn reads_fs(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsFs(self.descriptor) }
    }
    pub fn reads_ft(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsFt(self.descriptor) }
    }
    pub fn reads_fd(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_readsFd(self.descriptor) }
    }

    // @deprecated
    pub fn not_emited_by_compilers(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_notEmittedByCompilers(self.descriptor) }
    }
    pub fn not_emitted_by_compilers(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_notEmittedByCompilers(self.descriptor) }
    }
    pub fn can_be_hi(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_canBeHi(self.descriptor) }
    }
    pub fn can_be_lo(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_canBeLo(self.descriptor) }
    }
    pub fn does_link(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_doesLink(self.descriptor) }
    }
    pub fn does_dereference(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_doesDereference(self.descriptor) }
    }
    pub fn does_load(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_doesLoad(self.descriptor) }
    }
    pub fn does_store(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_doesStore(self.descriptor) }
    }
    pub fn maybe_is_move(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_maybeIsMove(self.descriptor) }
    }
    pub fn is_pseudo(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_isPseudo(self.descriptor) }
    }
    pub fn access_type(&self) -> access_type_enum::AccessType {
        unsafe { RabbitizerInstrDescriptor_getAccessType(self.descriptor) }
    }
    pub fn does_unsigned_memory_access(&self) -> bool {
        unsafe { RabbitizerInstrDescriptor_doesUnsignedMemoryAccess(self.descriptor) }
    }

    pub fn disassemble(
        &self,
        imm_override: Option<&str>,
        extra_l_just: core::ffi::c_int,
    ) -> String {
        let (imm_override_ptr, imm_override_len) = utils::c_string_from_str(imm_override);

        unsafe {
            let buffer_size =
                RabbitizerInstruction_getSizeForBuffer(self, imm_override_len, extra_l_just);

            let mut buffer: Vec<u8> = vec![0; buffer_size];
            let disassembled_size = RabbitizerInstruction_disassemble(
                self,
                buffer.as_mut_ptr() as *mut core::ffi::c_char,
                imm_override_ptr,
                imm_override_len,
                extra_l_just,
            );
            buffer.truncate(disassembled_size);

            String::from_utf8(buffer).unwrap()
        }
    }
}
