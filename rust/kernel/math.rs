// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Samsung Electronics Co., Ltd.
// Author: Michal Wilczynski <m.wilczynski@samsung.com>

//! Safe wrappers for kernel math helpers.
//!
//! This module provides safe, idiomatic Rust wrappers for C functions, whose
//! FFI bindings are auto-generated in the `bindings` crate.

use crate::bindings;

/// An extension trait that provides access to kernel math helpers on primitive integer types.
pub trait KernelMathExt: Sized {
    /// Multiplies self by `multiplier and divides by divisor.
    ///
    /// This wrapper around the kernel's `mul_u64_u64_div_u64` C helper ensures that no
    /// overflow occurs during the intermediate multiplication.
    ///
    /// # Returns
    /// * Some(result) if the division is successful.
    /// * None if the divisor is zero.
    fn mul_div(self, multiplier: Self, divisor: Self) -> Option<Self>;
}

impl KernelMathExt for u64 {
    fn mul_div(self, multiplier: u64, divisor: u64) -> Option<u64> {
        if divisor == 0 {
            return None;
        }
        // SAFETY: The C function `mul_u64_u64_div_u64` is safe to call because the divisor
        // is guaranteed to be non-zero. The FFI bindings use `u64`, matching our types.
        Some(unsafe { bindings::mul_u64_u64_div_u64(self, multiplier, divisor) })
    }
}
