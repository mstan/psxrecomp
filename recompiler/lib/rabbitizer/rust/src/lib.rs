/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

mod abi_enum;
mod access_type_enum;
pub mod config;
mod instr_category_enum;
mod instr_descriptor;
mod instr_id_enum;
mod instr_id_type_enum;
mod instr_suffix_enum;
mod instruction;
mod operand_type_enum;
mod opereand_type;
mod register_descriptor;
mod registers_enum;
mod registers_methods;
pub mod utils;

pub use abi_enum::Abi;
pub use access_type_enum::AccessType;
pub use instr_category_enum::InstrCategory;
pub use instr_descriptor::InstrDescriptor;
pub use instr_id_enum::InstrId;
pub use instr_id_type_enum::InstrIdType;
pub use instr_suffix_enum::InstrSuffix;
pub use instruction::Instruction;
pub use operand_type_enum::OperandType;
pub use register_descriptor::RegisterDescriptor;
pub use registers_enum::registers;

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default, Debug, Clone)]
    struct TestEntry {
        word: u32,
        imm_override: Option<String>,
        expected_str: String,
    }

    #[test]
    fn it_works() {
        assert_eq!(
            instruction::Instruction::new(
                0x8D4A7E18,
                0x80000000,
                instr_category_enum::InstrCategory::CPU
            )
            .disassemble(None, 0),
            "lw          $t2, 0x7E18($t2)".to_string()
        );
    }

    #[test]
    fn test_instruction_checks() {
        // jalr
        let instr = instruction::Instruction::new(
            0x00A0F809,
            0x80000000,
            instr_category_enum::InstrCategory::CPU,
        );

        assert!(instr.modifies_rd());
        assert_eq!(instr.get_rd_o32(), registers::GprO32::ra);
        assert_eq!(instr.disassemble(None, 0), "jalr        $a1".to_string());
    }

    #[test]
    fn test_get_operand_type() {
        let instr = instruction::Instruction::new(
            0x8D4A7E18,
            0x80000000,
            instr_category_enum::InstrCategory::CPU,
        );
        let operand = instr.get_operand_type(0);

        assert_eq!(operand.disassemble(&instr, None), "$t2".to_string());
    }

    #[test]
    fn test_operands_slice() {
        let instr = instruction::Instruction::new(
            0x8D4A7E18,
            0x80000000,
            instr_category_enum::InstrCategory::CPU,
        );
        let operands_slice = instr.get_operands_slice();

        assert_eq!(operands_slice.len(), 2);
    }

    #[test]
    fn test_register_descriptor() {
        assert!(registers::GprO32::a0
            .descriptor()
            .is_clobbered_by_func_call());
    }

    #[test]
    fn test_r3000gte_instructions() {
        let entries = [
            TestEntry {
                word: 0x4A180001,
                imm_override: None,
                expected_str: "RTPS".to_string(),
            },
            TestEntry {
                word: 0x4A280030,
                imm_override: None,
                expected_str: "RTPT".to_string(),
            },
            TestEntry {
                word: 0x4A680029,
                imm_override: None,
                expected_str: "DPCL".to_string(),
            },
            TestEntry {
                word: 0x4A780010,
                imm_override: None,
                expected_str: "DPCS".to_string(),
            },
            TestEntry {
                word: 0x4AF8002A,
                imm_override: None,
                expected_str: "DPCT".to_string(),
            },
            TestEntry {
                word: 0x4A980011,
                imm_override: None,
                expected_str: "INTPL".to_string(),
            },
            TestEntry {
                word: 0x4AC8041E,
                imm_override: None,
                expected_str: "NCS".to_string(),
            },
            TestEntry {
                word: 0x4AD80420,
                imm_override: None,
                expected_str: "NCT".to_string(),
            },
            TestEntry {
                word: 0x4AE80413,
                imm_override: None,
                expected_str: "NCDS".to_string(),
            },
            TestEntry {
                word: 0x4AF80416,
                imm_override: None,
                expected_str: "NCDT".to_string(),
            },
            TestEntry {
                word: 0x4B08041B,
                imm_override: None,
                expected_str: "NCCS".to_string(),
            },
            TestEntry {
                word: 0x4B18043F,
                imm_override: None,
                expected_str: "NCCT".to_string(),
            },
            TestEntry {
                word: 0x4B280414,
                imm_override: None,
                expected_str: "CDP".to_string(),
            },
            TestEntry {
                word: 0x4B38041C,
                imm_override: None,
                expected_str: "CC".to_string(),
            },
            TestEntry {
                word: 0x4B400006,
                imm_override: None,
                expected_str: "NCLIP".to_string(),
            },
            TestEntry {
                word: 0x4B58002D,
                imm_override: None,
                expected_str: "AVSZ3".to_string(),
            },
            TestEntry {
                word: 0x4B68002E,
                imm_override: None,
                expected_str: "AVSZ4".to_string(),
            },
            TestEntry {
                word: 0x4A400012,
                imm_override: None,
                expected_str: "MVMVA       0, 0, 0, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4AA00428,
                imm_override: None,
                expected_str: "SQR         0".to_string(),
            },
            TestEntry {
                word: 0x4B70000C,
                imm_override: None,
                expected_str: "OP          0".to_string(),
            },
            TestEntry {
                word: 0x4B90003D,
                imm_override: None,
                expected_str: "GPF         0".to_string(),
            },
            TestEntry {
                word: 0x4BA0003E,
                imm_override: None,
                expected_str: "GPL         0".to_string(),
            },
            TestEntry {
                word: 0x4A486012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 0, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A48E012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 1, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A496012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 2, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A49E012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 3, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A41E012,
                imm_override: None,
                expected_str: "MVMVA       0, 0, 3, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A480012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 0, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A488012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 1, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A490012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 2, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A498012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 3, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A482012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 0, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A48A012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 1, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A492012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 2, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A49A012,
                imm_override: None,
                expected_str: "MVMVA       1, 0, 3, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4A6412,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 0, 3, 1".to_string(),
            },
            TestEntry {
                word: 0x4A4A6012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 0, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4AE012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 1, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4B6012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 2, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4BE012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 3, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4A0012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 0, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4A8012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 1, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4B0012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 2, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4B8012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 3, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4A2012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 0, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4AA012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 1, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4B2012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 2, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4BA012,
                imm_override: None,
                expected_str: "MVMVA       1, 1, 3, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4DA412,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 3, 1, 1".to_string(),
            },
            TestEntry {
                word: 0x4A4C6012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 0, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4CE012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 1, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4D6012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 2, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4DE012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 3, 3, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4C0012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 0, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4C8012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 1, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4D0012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 2, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4D8012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 3, 0, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4C2012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 0, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4CA012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 1, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4D2012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 2, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4A4DA012,
                imm_override: None,
                expected_str: "MVMVA       1, 2, 3, 1, 0".to_string(),
            },
            TestEntry {
                word: 0x4AA80428,
                imm_override: None,
                expected_str: "SQR         1".to_string(),
            },
            TestEntry {
                word: 0x4AA80428,
                imm_override: None,
                expected_str: "SQR         1".to_string(),
            },
            TestEntry {
                word: 0x4B78000C,
                imm_override: None,
                expected_str: "OP          1".to_string(),
            },
            TestEntry {
                word: 0x4B70000C,
                imm_override: None,
                expected_str: "OP          0".to_string(),
            },
            TestEntry {
                word: 0x4B98003D,
                imm_override: None,
                expected_str: "GPF         1".to_string(),
            },
            TestEntry {
                word: 0x4B90003D,
                imm_override: None,
                expected_str: "GPF         0".to_string(),
            },
            TestEntry {
                word: 0x4BA8003E,
                imm_override: None,
                expected_str: "GPL         1".to_string(),
            },
            TestEntry {
                word: 0x4BA0003E,
                imm_override: None,
                expected_str: "GPL         0".to_string(),
            },
        ];

        for entry in entries.iter() {
            let instr = instruction::Instruction::new(
                entry.word,
                0x80000000,
                instr_category_enum::InstrCategory::R3000GTE,
            );

            assert_eq!(
                instr.disassemble(entry.imm_override.as_deref(), 0),
                entry.expected_str
            );
        }
    }
}
