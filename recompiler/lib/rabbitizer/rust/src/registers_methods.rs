/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

use crate::{registers_enum, RegisterDescriptor};

extern "C" {
    pub static mut RabbitizerRegister_GprO32_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_GprN32_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_Cop0_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_Cop1O32_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_Cop1N32_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_Cop1N64_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_Cop1Control_Names:
        [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_Cop2_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_RspGpr_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_RspCop0_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_RspCop2_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_RspCop2Control_Names:
        [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_RspVector_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_R5900VF_Names: [[*const core::ffi::c_char; 2usize]; 0usize];
    pub static mut RabbitizerRegister_R5900VI_Names: [[*const core::ffi::c_char; 2usize]; 0usize];

    /*
    pub fn RabbitizerRegister_getNameGpr(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameCop0(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameCop1(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameCop1Control(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameCop2(reg_value: u8) -> *const core::ffi::c_char;

    pub fn RabbitizerRegister_getNameRspGpr(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameRspCop0(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameRspCop2(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameRspCop2Control(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameRspVector(reg_value: u8) -> *const core::ffi::c_char;

    pub fn RabbitizerRegister_getNameR5900VF(reg_value: u8) -> *const core::ffi::c_char;
    pub fn RabbitizerRegister_getNameR5900VI(reg_value: u8) -> *const core::ffi::c_char;
    */

    pub static mut RabbitizerRegister_GprO32_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_GprN32_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_Cop0_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_Cop1O32_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_Cop1N32_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_Cop1N64_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_Cop1Control_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_Cop2_Descriptors: [RegisterDescriptor; 0usize];

    /* RSP */

    pub static mut RabbitizerRegister_RspGpr_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_RspCop0_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_RspCop2_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_RspCop2Control_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_RspVector_Descriptors: [RegisterDescriptor; 0usize];

    /* RSP */

    /* R5900 */

    pub static mut RabbitizerRegister_R5900VF_Descriptors: [RegisterDescriptor; 0usize];
    pub static mut RabbitizerRegister_R5900VI_Descriptors: [RegisterDescriptor; 0usize];

    /* R5900 */
}

impl registers_enum::registers::GprO32 {
    pub fn name(&self) -> &'static str {
        let reg_value: u32 = (*self).into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_GprO32_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_GprO32_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::GprN32 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_GprN32_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_GprN32_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::Cop0 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_Cop0_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_Cop0_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::Cop1O32 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_Cop1O32_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_Cop1O32_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::Cop1N32 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_Cop1N32_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_Cop1N32_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::Cop1N64 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_Cop1N64_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_Cop1N64_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::Cop1Control {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_Cop1Control_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_Cop1Control_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::Cop2 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_Cop2_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_Cop2_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::RspGpr {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_RspGpr_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_RspGpr_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::RspCop0 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_RspCop0_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_RspCop0_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::RspCop2 {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_RspCop2_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_RspCop2_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::RspCop2Control {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_RspCop2Control_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_RspCop2Control_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::RspVector {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_RspVector_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_RspVector_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::R5900VF {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe {
            std::ffi::CStr::from_ptr(RabbitizerRegister_R5900VF_Names[reg_value as usize][1])
                .to_str()
                .unwrap()
        }
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_R5900VF_Descriptors.get_unchecked(reg_value as usize) }
    }
}

impl registers_enum::registers::R5900VI {
    pub fn name(self) -> &'static str {
        let reg_value: u32 = self.into();

        unsafe { std::ffi::CStr::from_ptr(RabbitizerRegister_R5900VI_Names[reg_value as usize][1]) }
            .to_str()
            .unwrap()
    }

    pub fn descriptor(&self) -> &RegisterDescriptor {
        let reg_value: u32 = (*self).into();

        unsafe { RabbitizerRegister_R5900VI_Descriptors.get_unchecked(reg_value as usize) }
    }
}
