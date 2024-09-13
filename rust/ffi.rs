// SPDX-License-Identifier: GPL-2.0

//! Foreign function interface (FFI) types.
//!
//! This crate provides mapping from C primitive types to Rust ones.
//!
//! Rust core crate provides [`core::ffi`], which maps integer types to platform default C ABI.
//! Kernel does not use `core::ffi` so it can customise the mapping that deviates from platform
//! default.

#![no_std]

pub use core::ffi::*;
