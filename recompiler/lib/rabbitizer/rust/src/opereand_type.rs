/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

use crate::{instruction, operand_type_enum, utils};

extern "C" {
    fn RabbitizerOperandType_getBufferSize(
        operand: operand_type_enum::OperandType,
        instr: *const instruction::Instruction,
        immOverrideLength: utils::SizeT,
    ) -> utils::SizeT;

    fn RabbitizerOperandType_disassemble(
        operand: operand_type_enum::OperandType,
        instr: *const instruction::Instruction,
        dst: *mut core::ffi::c_char,
        immOverride: *const core::ffi::c_char,
        immOverrideLength: utils::SizeT,
    ) -> utils::SizeT;
}

impl operand_type_enum::OperandType {
    pub fn disassemble(
        &self,
        instr: &instruction::Instruction,
        imm_override: Option<&str>,
    ) -> String {
        let (imm_override_ptr, imm_override_len) = utils::c_string_from_str(imm_override);

        unsafe {
            let buffer_size = RabbitizerOperandType_getBufferSize(*self, instr, imm_override_len);

            let mut buffer: Vec<u8> = vec![0; buffer_size];
            let disassembled_size = RabbitizerOperandType_disassemble(
                *self,
                instr,
                buffer.as_mut_ptr() as *mut core::ffi::c_char,
                imm_override_ptr,
                imm_override_len,
            );
            buffer.truncate(disassembled_size);

            String::from_utf8(buffer).unwrap()
        }
    }
}
