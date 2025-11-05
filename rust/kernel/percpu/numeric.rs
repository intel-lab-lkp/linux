// SPDX-License-Identifier: GPL-2.0
//! Pin-hole optimizations for [`PerCpu<T>`] where T is a numeric type.

use super::*;
use core::arch::asm;

/// Represents a per-CPU variable that can be manipulated with machine-intrinsic numeric
/// operations.
pub struct PerCpuNumeric<'a, T> {
    // INVARIANT: `ptr.0` is a valid offset into the per-CPU area and is initialized on all CPUs
    // (since we don't have a CPU guard, we have to be pessimistic and assume we could be on any
    // CPU).
    ptr: &'a PerCpuPtr<T>,
}

macro_rules! impl_ops {
    ($ty:ty, $reg:tt) => {
        impl DynamicPerCpu<$ty> {
            /// Returns a [`PerCpuNumeric`] that can be used to manipulate the underlying per-CPU
            /// variable.
            #[inline]
            pub fn num(&mut self) -> PerCpuNumeric<'_, $ty> {
                // The invariant is satisfied because `DynamicPerCpu`'s invariant guarantees that
                // this pointer is valid and initialized on all CPUs.
                PerCpuNumeric { ptr: &self.ptr }
            }
        }
        impl StaticPerCpu<$ty> {
            /// Returns a [`PerCpuNumeric`] that can be used to manipulate the underlying per-CPU
            /// variable.
            #[inline]
            pub fn num(&mut self) -> PerCpuNumeric<'_, $ty> {
                // The invariant is satisfied because `StaticPerCpu`'s invariant guarantees that
                // this pointer is valid and initialized on all CPUs.
                PerCpuNumeric { ptr: &self.0 }
            }
        }

        impl PerCpuNumeric<'_, $ty> {
            /// Adds `rhs` to the per-CPU variable.
            #[inline]
            pub fn add(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of this type.
                unsafe {
                    asm!(
                        concat!("add gs:[{off}], {val:", $reg, "}"),
                        off = in(reg) self.ptr.0.cast::<*mut $ty>(),
                        val = in(reg) rhs,
                    );
                }
            }
        }
        impl PerCpuNumeric<'_, $ty> {
            /// Subtracts `rhs` from the per-CPU variable.
            #[inline]
            pub fn sub(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of this type.
                unsafe {
                    asm!(
                        concat!("sub gs:[{off}], {val:", $reg, "}"),
                        off = in(reg) self.ptr.0.cast::<*mut $ty>(),
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
            /// Returns a [`PerCpuNumeric`] that can be used to manipulate the underlying per-CPU
            /// variable.
            #[inline]
            pub fn num(&mut self) -> PerCpuNumeric<'_, $ty> {
                // The invariant is satisfied because `DynamicPerCpu`'s invariant guarantees that
                // this pointer is valid and initialized on all CPUs.
                PerCpuNumeric { ptr: &self.ptr }
            }
        }
        impl StaticPerCpu<$ty> {
            /// Returns a [`PerCpuNumeric`] that can be used to manipulate the underlying per-CPU
            /// variable.
            #[inline]
            pub fn num(&mut self) -> PerCpuNumeric<'_, $ty> {
                // The invariant is satisfied because `DynamicPerCpu`'s invariant guarantees that
                // this pointer is valid and initialized on all CPUs.
                PerCpuNumeric { ptr: &self.0 }
            }
        }

        impl PerCpuNumeric<'_, $ty> {
            /// Adds `rhs` to the per-CPU variable.
            #[inline]
            pub fn add(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of this type.
                unsafe {
                    asm!(
                        concat!("add gs:[{off}], {val}"),
                        off = in(reg) self.ptr.0.cast::<*mut $ty>(),
                        val = in(reg_byte) rhs,
                    );
                }
            }
        }
        impl PerCpuNumeric<'_, $ty> {
            /// Subtracts `rhs` from the per-CPU variable.
            #[inline]
            pub fn sub(&mut self, rhs: $ty) {
                // SAFETY: `self.ptr.0` is a valid offset into the per-CPU area (i.e., valid as a
                // pointer relative to the `gs` segment register) by the invariants of this type.
                unsafe {
                    asm!(
                        concat!("sub gs:[{off}], {val}"),
                        off = in(reg) self.ptr.0.cast::<*mut $ty>(),
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
