/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

#[repr(C)]
#[derive(Debug, Copy, Clone)]
#[allow(non_camel_case_types)]
pub struct RegisterDescriptor {
    is_clobbered_by_func_call: bool,
    is_reserved: bool,
    is_kernel: bool,
    is_zero: bool,
    is_at: bool,
    is_return_reg: bool,
    is_ra: bool,
    is_stack_pointer: bool,
    is_gp: bool,
    is_temp: bool,
    is_arg: bool,
    is_saved: bool,
}

impl RegisterDescriptor {
    pub fn is_clobbered_by_func_call(&self) -> bool {
        self.is_clobbered_by_func_call
    }
    pub fn is_reserved(&self) -> bool {
        self.is_reserved
    }
    pub fn is_kernel(&self) -> bool {
        self.is_kernel
    }
    pub fn is_zero(&self) -> bool {
        self.is_zero
    }
    pub fn is_at(&self) -> bool {
        self.is_at
    }
    pub fn is_return_reg(&self) -> bool {
        self.is_return_reg
    }
    pub fn is_ra(&self) -> bool {
        self.is_ra
    }
    pub fn is_stack_pointer(&self) -> bool {
        self.is_stack_pointer
    }
    pub fn is_gp(&self) -> bool {
        self.is_gp
    }
    pub fn is_temp(&self) -> bool {
        self.is_temp
    }
    pub fn is_arg(&self) -> bool {
        self.is_arg
    }
    pub fn is_saved(&self) -> bool {
        self.is_saved
    }
}
