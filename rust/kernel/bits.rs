// SPDX-License-Identifier: GPL-2.0

//! Bit manipulation macros.
//!
//! C header: [`include/linux/bits.h`](srctree/include/linux/bits.h)

use crate::prelude::*;
use core::ops::RangeInclusive;
use macros::paste;

macro_rules! impl_bit_fn {
    (
        $ty:ty
    ) => {
        paste! {
            /// Computes `1 << n` if `n` is in bounds, i.e.: if `n` is smaller than
            /// the maximum number of bits supported by the type.
            ///
            /// Returns [`None`] otherwise.
            #[inline]
            pub fn [<checked_bit_ $ty>](n: u32) -> Option<$ty> {
                (1 as $ty).checked_shl(n)
            }

            /// Computes `1 << n` by performing a compile-time assertion that `n` is
            /// in bounds.
            ///
            /// This version is the default and should be used if `n` is known at
            /// compile time.
            #[inline]
            pub const fn [<bit_ $ty>](n: u32) -> $ty {
                build_assert!(n < <$ty>::BITS);
                (1 as $ty) << n
            }
        }
    };
}

impl_bit_fn!(u64);
impl_bit_fn!(u32);
impl_bit_fn!(u16);
impl_bit_fn!(u8);

macro_rules! impl_genmask_fn {
    (
        $ty:ty,
        $(#[$genmask_ex:meta])*
    ) => {
        paste! {
            /// Creates a compile-time contiguous bitmask for the given range by
            /// validating the range at runtime.
            ///
            /// Returns [`None`] if the range is invalid, i.e.: if the start is
            /// greater than or equal to the end.
            #[inline]
            pub fn [<genmask_checked_ $ty>](range: RangeInclusive<u32>) -> Option<$ty> {
                let start = *range.start();
                let end = *range.end();

                if start >= end || end >= <$ty>::BITS {
                    return None;
                }

                let high = [<checked_bit_ $ty>](end)?;
                let low = [<checked_bit_ $ty>](start)?;
                Some((high | (high - 1)) & !(low - 1))
            }

            /// Creates a compile-time contiguous bitmask for the given range by
            /// performing a compile-time assertion that the range is valid.
            ///
            /// This version is the default and should be used if the range is known
            /// at compile time.
            $(#[$genmask_ex])*
            #[inline]
            pub const fn [<genmask_ $ty>](range: RangeInclusive<u32>) -> $ty {
                let start = *range.start();
                let end = *range.end();

                build_assert!(start < end);
                build_assert!(end < <$ty>::BITS);

                let high = [<bit_ $ty>](end);
                let low = [<bit_ $ty>](start);
                (high | (high - 1)) & !(low - 1)
            }
        }
    };
}

impl_genmask_fn!(
    u64,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u64;
    /// let mask = genmask_u64(21..=39);
    /// assert_eq!(mask, 0x000000ffffe00000);
    /// ```
);

impl_genmask_fn!(
    u32,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u32;
    /// let mask = genmask_u32(0..=9);
    /// assert_eq!(mask, 0x000003ff);
    /// ```
);

impl_genmask_fn!(
    u16,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u16;
    /// let mask = genmask_u16(0..=9);
    /// assert_eq!(mask, 0x000003ff);
    /// ```
);

impl_genmask_fn!(
    u8,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u8;
    /// let mask = genmask_u8(0..=7);
    /// assert_eq!(mask, 0x000000ff);
    /// ```
);
