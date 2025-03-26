// SPDX-License-Identifier: GPL-2.0

//! Bit manipulation macros.
//!
//! C header: [`include/linux/bits.h`](srctree/include/linux/bits.h)

/// Produces a literal where bit `n` is set.
///
/// Equivalent to the kernel's `BIT` macro.
pub const fn bit_u64(n: u32) -> u64 {
    1u64 << n as u64
}

/// Produces a literal where bit `n` is set.
///
/// Equivalent to the kernel's `BIT` macro.
pub const fn bit_u32(n: u32) -> u32 {
    1u32 << n
}

/// Produces a literal where bit `n` is set.
///
/// Equivalent to the kernel's `BIT` macro.
pub const fn bit_u16(n: u32) -> u16 {
    1u16 << n as u16
}

/// Produces a literal where bit `n` is set.
///
/// Equivalent to the kernel's `BIT` macro.
pub const fn bit_u8(n: u32) -> u8 {
    1u8 << n as u8
}

/// Create a contiguous bitmask starting at bit position `l` and ending at
/// position `h`, where `h >= l`.
///
/// # Examples
/// ```
///     use kernel::bits::genmask_u64;
///     let mask = genmask_u64(39, 21);
///     assert_eq!(mask, 0x000000ffffe00000);
/// ```
///
pub const fn genmask_u64(h: u32, l: u32) -> u64 {
    assert!(h >= l);
    (!0u64 - (1u64 << l) + 1) & (!0u64 >> (64 - 1 - h))
}

/// Create a contiguous bitmask starting at bit position `l` and ending at
/// position `h`, where `h >= l`.
///
/// # Examples
/// ```
///     use kernel::bits::genmask_u32;
///     let mask = genmask_u32(9, 0);
///     assert_eq!(mask, 0x000003ff);
/// ```
///
pub const fn genmask_u32(h: u32, l: u32) -> u32 {
    assert!(h >= l);
    (!0u32 - (1u32 << l) + 1) & (!0u32 >> (32 - 1 - h))
}

/// Create a contiguous bitmask starting at bit position `l` and ending at
/// position `h`, where `h >= l`.
///
/// # Examples
/// ```
///     use kernel::bits::genmask_u16;
///     let mask = genmask_u16(9, 0);
///     assert_eq!(mask, 0x000003ff);
/// ```
///
pub const fn genmask_u16(h: u32, l: u32) -> u16 {
    assert!(h >= l);
    (!0u16 - (1u16 << l) + 1) & (!0u16 >> (16 - 1 - h))
}

/// Create a contiguous bitmask starting at bit position `l` and ending at
/// position `h`, where `h >= l`.
///
/// # Examples
/// ```
///     use kernel::bits::genmask_u8;
///     let mask = genmask_u8(7, 0);
///     assert_eq!(mask, 0x000000ff);
/// ```
///
pub const fn genmask_u8(h: u32, l: u32) -> u8 {
    assert!(h >= l);
    (!0u8 - (1u8 << l) + 1) & (!0u8 >> (8 - 1 - h))
}
