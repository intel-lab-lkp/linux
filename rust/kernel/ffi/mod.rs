// SPDX-License-Identifier: GPL-2.0

//! FFI infrastructure for interfacing with C code.

// Re-export C type definitions from the `ffi` crate so that existing
// `kernel::ffi::c_int` etc. paths continue to work.
pub use ::ffi::*;

pub mod clist;
