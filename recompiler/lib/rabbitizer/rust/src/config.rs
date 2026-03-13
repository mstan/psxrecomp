/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

use crate::abi_enum;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
#[allow(non_camel_case_types)]
pub struct RegisterNames {
    pub named_registers: bool,
    pub gpr_abi_names: abi_enum::Abi,
    pub fpr_abi_names: abi_enum::Abi,
    pub user_fpc_csr: bool,
    pub vr4300_cop0_named_registers: bool,
    pub vr4300_rsp_cop0_named_registers: bool,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
#[allow(non_camel_case_types)]
pub struct PseudoInstr {
    pub enable_pseudos: bool,
    pub pseudo_beqz: bool,
    pub pseudo_bnez: bool,
    pub pseudo_b: bool,
    pub pseudo_move: bool,
    pub pseudo_not: bool,
    pub pseudo_negu: bool,
    pub pseudo_bal: bool,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
#[allow(non_camel_case_types)]
pub struct ToolchainTweaks {
    pub treat_j_as_unconditional_branch: bool,
    pub sn64_div_fix: bool,
    pub gnu_mode: bool,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
#[allow(non_camel_case_types)]
pub struct Misc {
    pub opcode_l_just: i32,
    pub unknown_instr_comment: bool,
    pub omit_0x_on_small_imm: bool,
    pub upper_case_imm: bool,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
#[allow(non_camel_case_types)]
pub struct Config {
    pub reg_names: RegisterNames,
    pub pseudos: PseudoInstr,
    pub toolchain_tweaks: ToolchainTweaks,
    pub misc: Misc,
}

extern "C" {
    pub static mut RabbitizerConfig_Cfg: Config;
}
