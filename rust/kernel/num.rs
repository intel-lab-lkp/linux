// SPDX-License-Identifier: GPL-2.0

//! Numerical and binary utilities for primitive types.

use core::ops::Add;

/// Trait for performing a checked addition that returns `None` if the operation would overflow.
///
/// This trait exists in order to represent scalar types already having a `checked_add` method in
/// generic code.
pub trait CheckedAdd: Sized + Add<Self, Output = Self> {
    /// Computes `self + rhs`, returning `None` if an overflow would occur.
    fn checked_add(self, rhs: Self) -> Option<Self>;
}

macro_rules! impl_checked_add {
    ($($t:ty),*) => {
        $(
        impl CheckedAdd for $t {
            fn checked_add(self, rhs: Self) -> Option<Self> {
                self.checked_add(rhs)
            }
        }
        )*
    };
}

impl_checked_add!(u8, u16, u32, u64, usize, i8, i16, i32, i64, isize);
