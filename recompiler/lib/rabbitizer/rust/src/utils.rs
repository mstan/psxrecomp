/* SPDX-FileCopyrightText: © 2022-2023 Decompollaborate */
/* SPDX-License-Identifier: MIT */

pub type SizeT = usize;

pub fn c_string_from_str(str: Option<&str>) -> (*const core::ffi::c_char, SizeT) {
    if let Some(str) = str {
        (str.as_ptr() as *const core::ffi::c_char, str.len())
    } else {
        (std::ptr::null(), 0)
    }
}

pub fn mask(v: u32, w: u32) -> u32 {
    v & ((1 << w) - 1)
}

pub fn shiftl(v: u32, s: u32, w: u32) -> u32 {
    mask(v, w) << s
}

pub fn shiftr(v: u32, s: u32, w: u32) -> u32 {
    mask(v >> s, w)
}

pub fn convert_option_string_to_option_str(input: &Option<String>) -> Option<&str> {
    match input {
        None => None,
        Some(x) => Some(x.as_str()),
    }
}
