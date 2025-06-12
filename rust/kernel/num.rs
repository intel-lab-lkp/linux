// SPDX-License-Identifier: GPL-2.0

//! Numerical and binary utilities for primitive types.

use crate::build_assert;
use core::borrow::Borrow;
use core::fmt::Debug;
use core::hash::Hash;
use core::ops::Deref;

/// An unsigned integer which is guaranteed to be a power of 2.
#[derive(Debug, Clone, Copy)]
#[repr(transparent)]
pub struct PowerOfTwo<T>(T);

macro_rules! power_of_two_impl {
    ($($t:ty),+) => {
        $(
            impl PowerOfTwo<$t> {
                /// Validates that `v` is a power of two at build-time, and returns it wrapped into
                /// `PowerOfTwo`.
                ///
                /// A build error is triggered if `v` cannot be asserted to be a power of two.
                ///
                /// # Examples
                ///
                /// ```
                /// use kernel::num::PowerOfTwo;
                ///
                /// let v = PowerOfTwo::<u32>::new(256);
                /// assert_eq!(v.value(), 256);
                /// ```
                #[inline(always)]
                pub const fn new(v: $t) -> Self {
                    build_assert!(v.count_ones() == 1);
                    Self(v)
                }

                /// Validates that `v` is a power of two at runtime, and returns it wrapped into
                /// `PowerOfTwo`.
                ///
                /// `None` is returned if `v` was not a power of two.
                ///
                /// # Examples
                ///
                /// ```
                /// use kernel::num::PowerOfTwo;
                ///
                /// assert_eq!(PowerOfTwo::<u32>::try_new(16).unwrap().value(), 16);
                /// assert_eq!(PowerOfTwo::<u32>::try_new(15), None);
                /// ```
                #[inline(always)]
                pub const fn try_new(v: $t) -> Option<Self> {
                    match v.count_ones() {
                        1 => Some(Self(v)),
                        _ => None,
                    }
                }

                /// Returns the value of this instance.
                ///
                /// It is guaranteed to be a power of two.
                ///
                /// # Examples
                ///
                /// ```
                /// use kernel::num::PowerOfTwo;
                ///
                /// let v = PowerOfTwo::<u32>::new(256);
                /// assert_eq!(v.value(), 256);
                /// ```
                #[inline(always)]
                pub const fn value(&self) -> $t {
                    self.0
                }

                /// Returns the mask corresponding to `self.value() - 1`.
                #[inline(always)]
                pub const fn mask(&self) -> $t {
                    self.0.wrapping_sub(1)
                }

                /// Aligns `self` down to `alignment`.
                ///
                /// # Examples
                ///
                /// ```
                /// use kernel::num::PowerOfTwo;
                ///
                /// assert_eq!(PowerOfTwo::<u32>::new(0x1000).align_down(0x4fff), 0x4000);
                /// ```
                #[inline(always)]
                pub const fn align_down(self, value: $t) -> $t {
                    value & !self.mask()
                }

                /// Aligns `value` up to `self`.
                ///
                /// Wraps around to `0` if the requested alignment pushes the result above the
                /// type's limits.
                ///
                /// # Examples
                ///
                /// ```
                /// use kernel::num::PowerOfTwo;
                ///
                /// assert_eq!(PowerOfTwo::<u32>::new(0x1000).align_up(0x4fff), 0x5000);
                /// assert_eq!(PowerOfTwo::<u32>::new(0x1000).align_up(0x4000), 0x4000);
                /// assert_eq!(PowerOfTwo::<u32>::new(0x1000).align_up(0x0), 0x0);
                /// assert_eq!(PowerOfTwo::<u16>::new(0x100).align_up(0xffff), 0x0);
                /// ```
                #[inline(always)]
                pub const fn align_up(self, value: $t) -> $t {
                    self.align_down(value.wrapping_add(self.mask()))
                }
            }
        )+
    };
}

power_of_two_impl!(usize, u8, u16, u32, u64, u128);

impl<T> Deref for PowerOfTwo<T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl<T> PartialEq for PowerOfTwo<T>
where
    T: PartialEq,
{
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}

impl<T> Eq for PowerOfTwo<T> where T: Eq {}

impl<T> PartialOrd for PowerOfTwo<T>
where
    T: PartialOrd,
{
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        self.0.partial_cmp(&other.0)
    }
}

impl<T> Ord for PowerOfTwo<T>
where
    T: Ord,
{
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.0.cmp(&other.0)
    }
}

impl<T> Hash for PowerOfTwo<T>
where
    T: Hash,
{
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        self.0.hash(state);
    }
}

impl<T> Borrow<T> for PowerOfTwo<T> {
    fn borrow(&self) -> &T {
        &self.0
    }
}

macro_rules! impl_fls {
    ($($t:ty),+) => {
        $(
            ::kernel::macros::paste! {
            /// Find Last Set Bit: return the 1-based index of the last (i.e. most significant) set
            /// bit in `v`.
            ///
            /// Equivalent to the C `fls` function.
            ///
            /// # Examples
            ///
            /// ```
            /// use kernel::num::fls_u32;
            ///
            /// assert_eq!(fls_u32(0x0), 0);
            /// assert_eq!(fls_u32(0x1), 1);
            /// assert_eq!(fls_u32(0x10), 5);
            /// assert_eq!(fls_u32(0xffff), 16);
            /// assert_eq!(fls_u32(0x8000_0000), 32);
            /// ```
            #[inline(always)]
            pub const fn [<fls_ $t>](v: $t) -> u32 {
                $t::BITS - v.leading_zeros()
            }
            }
        )+
    };
}

impl_fls!(usize, u8, u16, u32, u64, u128);
