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
        $(#[$genmask_checked_ex:meta])*,
        $(#[$genmask_ex:meta])*
    ) => {
        paste! {
            /// Creates a contiguous bitmask for the given range by validating
            /// the range at runtime.
            ///
            /// Returns [`None`] if the range is invalid, i.e.: if the start is
            /// greater than or equal to the end or if the range is outside of
            /// the representable range for the type.
            $(#[$genmask_checked_ex])*
            #[inline]
            pub fn [<genmask_checked_ $ty>](range: RangeInclusive<u32>) -> Option<$ty> {
                let start = *range.start();
                let end = *range.end();

                if start > end {
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

                build_assert!(start <= end);

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
    /// The example below highlights the default use case, i.e., when the range
    /// is being built from non-constant values, which are represented here as
    /// the function arguments `a` and `b`.
    ///
    /// ```
    /// fn build_mask(a: u32, b: u32) -> Option<u64> {
    ///     # use kernel::bits::genmask_checked_u64;
    ///     // Ensures that a valid mask can be constructed for the range
    ///     // `a..=b` by performing runtime checks.
    ///     genmask_checked_u64(a..=b)
    /// }
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u64;
    /// let mask = genmask_checked_u64(0..=0);
    /// assert_eq!(mask, Some(0b1));
    /// ```
    ///
    /// This example tests the edge case in which all bits are supposed to be
    /// set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u64;
    /// let mask = genmask_checked_u64(0..=63);
    /// assert_eq!(mask, Some(u64::MAX));
    /// ```
    ,
    /// # Examples
    ///
    /// This example highlights the default use case, i.e., when the range can
    /// be built from two constant values.
    ///
    /// ```
    /// # use kernel::bits::genmask_u64;
    /// let mask = genmask_u64(21..=39);
    /// assert_eq!(mask, 0x0000_00ff_ffe0_0000);
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u64;
    /// let mask = genmask_u64(0..=0);
    /// assert_eq!(mask, 0b1);
    /// ```
    ///
    /// This example tests an edge case where all bits are supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u64;
    /// let mask = genmask_u64(0..=63);
    /// assert_eq!(mask, u64::MAX);
    /// ```
);

impl_genmask_fn!(
    u32,
    /// # Examples
    ///
    /// The example below highlights the default use case, i.e., when the range
    /// is being built from non-constant values, which are represented here as
    /// the function arguments `a` and `b`.
    ///
    /// ```
    /// fn build_mask(a: u32, b: u32) -> Option<u32> {
    ///     # use kernel::bits::genmask_checked_u32;
    ///     // Ensures that a valid mask can be constructed for the range
    ///     // `a..=b` by performing runtime checks.
    ///     genmask_checked_u32(a..=b)
    /// }
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u32;
    /// let mask = genmask_checked_u32(0..=0);
    /// assert_eq!(mask, Some(0b1));
    /// ```
    ///
    /// This example tests the edge case in which all bits are supposed to be
    /// set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u32;
    /// let mask = genmask_checked_u32(0..=31);
    /// assert_eq!(mask, Some(u32::MAX));
    /// ```
    ,
    /// # Examples
    ///
    /// This example highlights the default use case, i.e., when the range can
    /// be built from two constant values.
    ///
    /// ```
    /// # use kernel::bits::genmask_u32;
    /// let mask = genmask_u32(21..=31);
    /// assert_eq!(mask, 0xffe0_0000);
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u32;
    /// let mask = genmask_u32(0..=0);
    /// assert_eq!(mask, 0b1);
    /// ```
    ///
    /// This example tests an edge case where all bits are supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u32;
    /// let mask = genmask_u32(0..=31);
    /// assert_eq!(mask, u32::MAX);
    /// ```
);

impl_genmask_fn!(
    u16,
    /// # Examples
    ///
    /// The example below highlights the default use case, i.e., when the range
    /// is being built from non-constant values, which are represented here as
    /// the function arguments `a` and `b`.
    ///
    /// ```
    /// fn build_mask(a: u32, b: u32) -> Option<u16> {
    ///     # use kernel::bits::genmask_checked_u16;
    ///     // Ensures that a valid mask can be constructed for the range
    ///     // `a..=b` by performing runtime checks.
    ///     genmask_checked_u16(a..=b)
    /// }
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u16;
    /// let mask = genmask_checked_u16(0..=0);
    /// assert_eq!(mask, Some(0b1));
    /// ```
    ///
    /// This example tests the edge case in which all bits are supposed to be
    /// set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u16;
    /// let mask = genmask_checked_u16(0..=15);
    /// assert_eq!(mask, Some(u16::MAX));
    /// ```
    ,
    /// # Examples
    ///
    /// This example highlights the default use case, i.e., when the range can
    /// be built from two constant values.
    ///
    /// ```
    /// # use kernel::bits::genmask_u16;
    /// let mask = genmask_u16(6..=15);
    /// assert_eq!(mask, 0xffc0);
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    /// ```
    /// # use kernel::bits::genmask_u16;
    /// let mask = genmask_u16(0..=0);
    /// assert_eq!(mask, 0b1);
    /// ```
    ///
    /// This example tests an edge case where all bits are supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u16;
    /// let mask = genmask_u16(0..=15);
    /// assert_eq!(mask, u16::MAX);
    /// ```
);

impl_genmask_fn!(
    u8,
    /// # Examples
    ///
    /// The example below highlights the default use case, i.e., when the range
    /// is being built from non-constant values, which are represented here as
    /// the function arguments `a` and `b`.
    ///
    /// ```
    /// fn build_mask(a: u32, b: u32) -> Option<u8> {
    ///     # use kernel::bits::genmask_checked_u8;
    ///     // Ensures that a valid mask can be constructed for the range
    ///     // `a..=b` by performing runtime checks.
    ///     genmask_checked_u8(a..=b)
    /// }
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u8;
    /// let mask = genmask_checked_u8(0..=0);
    /// assert_eq!(mask, Some(0b1));
    /// ```
    ///
    /// This example tests the edge case in which all bits are supposed to be
    /// set.
    ///
    /// ```
    /// # use kernel::bits::genmask_checked_u8;
    /// let mask = genmask_checked_u8(0..=7);
    /// assert_eq!(mask, Some(u8::MAX));
    /// ```
    ,
    /// # Examples
    ///
    /// This example highlights the default use case, i.e., when the range can
    /// be built from two constant values.
    ///
    /// ```
    /// # use kernel::bits::genmask_u8;
    /// let mask = genmask_u8(6..=7);
    /// assert_eq!(mask, 0xc0);
    /// ```
    ///
    /// This example tests an edge case where only the first bit is
    /// supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u8;
    /// let mask = genmask_u8(0..=0);
    /// assert_eq!(mask, 0b1);
    /// ```
    ///
    /// This example tests an edge case where all bits are supposed to be set.
    ///
    /// ```
    /// # use kernel::bits::genmask_u8;
    /// let mask = genmask_u8(0..=7);
    /// assert_eq!(mask, u8::MAX);
    /// ```
);
