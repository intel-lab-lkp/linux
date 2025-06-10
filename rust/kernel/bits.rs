// SPDX-License-Identifier: GPL-2.0

//! Bit manipulation macros.
//!
//! C header: [`include/linux/bits.h`](srctree/include/linux/bits.h)

use crate::build_assert;
use core::ops::Range;

macro_rules! impl_bit_fn {
    (
        $checked_name:ident, $unbounded_name:ident, $const_name:ident, $ty:ty
    ) => {
        /// Computes `1 << n` if `n` is in bounds, i.e.: if `n` is smaller than
        /// the maximum number of bits supported by the type.
        ///
        /// Returns [`None`] otherwise.
        #[inline]
        pub fn $checked_name(n: u32) -> Option<$ty> {
            (1 as $ty) .checked_shl(n)
        }

        /// Computes `1 << n` if `n` is in bounds, i.e.: if `n` is smaller than
        /// the maximum number of bits supported by the type.
        ///
        /// Returns `0` otherwise.
        ///
        /// This is a convenience, as [`Option::unwrap_or`] cannot be used in
        /// const-context.
        #[inline]
        pub fn $unbounded_name(n: u32) -> $ty {
            match $checked_name(n) {
                Some(v) => v,
                None => 0,
            }
        }

        /// Computes `1 << n` by performing a compile-time assertion that `n` is
        /// in bounds.
        ///
        /// This version is the default and should be used if `n` is known at
        /// compile time.
        #[inline]
        pub const fn $const_name(n: u32) -> $ty {
            build_assert!(n < <$ty>::BITS);
            1 as $ty << n
        }
    };
}

impl_bit_fn!(checked_bit_u64, unbounded_bit_u64, bit_u64, u64);
impl_bit_fn!(checked_bit_u32, unbounded_bit_u32, bit_u32, u32);
impl_bit_fn!(checked_bit_u16, unbounded_bit_u16, bit_u16, u16);
impl_bit_fn!(checked_bit_u8, unbounded_bit_u8, bit_u8, u8);

macro_rules! impl_genmask_fn {
    (
        $ty:ty, $checked_bit:ident, $bit:ident, $genmask:ident, $genmask_checked:ident, $genmask_unbounded:ident,
        $(#[$genmask_ex:meta])*
    ) => {
        /// Creates a compile-time contiguous bitmask for the given range by
        /// validating the range at runtime.
        ///
        /// Returns [`None`] if the range is invalid, i.e.: if the start is
        /// greater than or equal to the end.
        #[inline]
        pub fn $genmask_checked(range: Range<u32>) -> Option<$ty> {
            if range.start >= range.end || range.end > <$ty>::BITS {
                return None;
            }
            let high = $checked_bit(range.end)?;
            let low = $checked_bit(range.start)?;
            Some((high | (high - 1)) & !(low - 1))
        }

        /// Creates a compile-time contiguous bitmask for the given range by
        /// validating the range at runtime.
        ///
        /// Returns `0` if the range is invalid, i.e.: if the start is greater
        /// than or equal to the end.
        #[inline]
        pub fn $genmask_unbounded(range: Range<u32>) -> $ty {
            match $genmask_checked(range) {
                Some(v) => v,
                None => 0,
            }
        }

        /// Creates a compile-time contiguous bitmask for the given range by
        /// performing a compile-time assertion that the range is valid.
        ///
        /// This version is the default and should be used if the range is known
        /// at compile time.
        $(#[$genmask_ex])*
        #[inline]
        pub const fn $genmask(range: Range<u32>) -> $ty {
            build_assert!(range.start < range.end);
            build_assert!(range.end <= <$ty>::BITS);
            let high = $bit(range.end);
            let low = $bit(range.start);
            (high | (high - 1)) & !(low - 1)
        }
    };
}

impl_genmask_fn!(
    u64,
    checked_bit_u64,
    bit_u64,
    genmask_u64,
    genmask_checked_u64,
    genmask_unbounded_u64,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u64;
    /// let mask = genmask_u64(21..39);
    /// assert_eq!(mask, 0x000000ffffe00000);
    /// ```
);

impl_genmask_fn!(
    u32,
    checked_bit_u32,
    bit_u32,
    genmask_u32,
    genmask_checked_u32,
    genmask_unbounded_u32,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u32;
    /// let mask = genmask_u32(0..9);
    /// assert_eq!(mask, 0x000003ff);
    /// ```
);

impl_genmask_fn!(
    u16,
    checked_bit_u16,
    bit_u16,
    genmask_u16,
    genmask_checked_u16,
    genmask_unbounded_u16,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u16;
    /// let mask = genmask_u16(0..9);
    /// assert_eq!(mask, 0x000003ff);
    /// ```
);

impl_genmask_fn!(
    u8,
    checked_bit_u8,
    bit_u8,
    genmask_u8,
    genmask_checked_u8,
    genmask_unbounded_u8,
    /// # Examples
    ///
    /// ```
    /// # use kernel::bits::genmask_u8;
    /// let mask = genmask_u8(0..7);
    /// assert_eq!(mask, 0x000000ff);
    /// ```
);
