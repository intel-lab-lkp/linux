// SPDX-License-Identifier: GPL-2.0
//! Pin-hole optimizations for PerCpu<T> where T is a numeric type.

use crate::percpu::*;
use core::arch::asm;

/// Represents a per-CPU variable that can be manipulated with machine-intrinsic numeric
/// operations.
pub struct PerCpuNumeric<'a, T> {
    ptr: &'a PerCpuPtr<T>,
}

macro_rules! impl_ops {
    ($ty:ty, $reg:tt) => {
        impl DynamicPerCpu<$ty> {
            /// Returns a `PerCpuNumeric` that can be used to manipulate the underlying per-CPU variable.
            pub fn num(&self) -> PerCpuNumeric<'_, $ty> {
                PerCpuNumeric { ptr: &self.alloc.0 }
            }
        }
        impl StaticPerCpu<$ty> {
            /// Returns a `PerCpuNumeric` that can be used to manipulate the underlying per-CPU variable.
            pub fn num(&self) -> PerCpuNumeric<'_, $ty> {
                PerCpuNumeric { ptr: &self.0 }
            }
        }

        impl PerCpuNumeric<'_, $ty> {
            /// Adds `rhs` to the per-CPU variable.
            pub fn add(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of PerCpu.
                unsafe {
                    asm!(
                        concat!("add gs:[{off}], {val:", $reg, "}"),
                        off = in(reg) self.ptr.0 as *mut $ty,
                        val = in(reg) rhs,
                    );
                }
            }
        }
        impl PerCpuNumeric<'_, $ty> {
            /// Subtracts `rhs` from the per-CPU variable.
            pub fn sub(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of PerCpu.
                unsafe {
                    asm!(
                        concat!("sub gs:[{off}], {val:", $reg, "}"),
                        off = in(reg) self.ptr.0 as *mut $ty,
                        val = in(reg) rhs,
                    );
                }
            }
        }
    };
}

macro_rules! impl_ops_byte {
    ($ty:ty) => {
        impl DynamicPerCpu<$ty> {
            /// Returns a `PerCpuNumeric` that can be used to manipulate the underlying per-CPU
            /// variable.
            pub fn num(&self) -> PerCpuNumeric<'_, $ty> {
                PerCpuNumeric { ptr: &self.ptr }
            }
        }
        impl StaticPerCpu<$ty> {
            /// Returns a `PerCpuNumeric` that can be used to manipulate the underlying per-CPU
            /// variable.
            pub fn num(&self) -> PerCpuNumeric<'_, $ty> {
                PerCpuNumeric { ptr: &self.0 }
            }
        }

        impl PerCpuNumeric<'_, $ty> {
            /// Adds `rhs` to the per-CPU variable.
            pub fn add(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of PerCpu.
                unsafe {
                    asm!(
                        concat!("add gs:[{off}], {val}"),
                        off = in(reg) self.ptr.0 as *mut $ty,
                        val = in(reg_byte) rhs,
                    );
                }
            }
        }
        impl PerCpuNumeric<'_, $ty> {
            /// Subtracts `rhs` from the per-CPU variable.
            pub fn sub(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of PerCpu.
                unsafe {
                    asm!(
                        concat!("sub gs:[{off}], {val}"),
                        off = in(reg) self.ptr.0 as *mut $ty,
                        val = in(reg_byte) rhs,
                    );
                }
            }
        }
    };
}

impl_ops_byte!(i8);
impl_ops!(i16, "x");
impl_ops!(i32, "e");
impl_ops!(i64, "r");
impl_ops!(isize, "r");

impl_ops_byte!(u8);
impl_ops!(u16, "x");
impl_ops!(u32, "e");
impl_ops!(u64, "r");
impl_ops!(usize, "r");
