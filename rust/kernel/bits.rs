// SPDX-License-Identifier: GPL-2.0

//! Bit manipulation macros.
//!
//! C header: [`include/linux/bits.h`](srctree/include/linux/bits.h)

/// Produces a literal where bit `n` is set.
///
/// Equivalent to the kernel's `BIT` macro.
///
#[macro_export]
macro_rules! bit {
    ($n:expr) => {
        (1 << $n)
    };
}

/// Create a contiguous bitmask starting at bit position `l` and ending at
/// position `h`, where `h >= l`.
///
/// # Examples
/// ```
///     use kernel::genmask;
///     let mask = genmask!(39, 21);
///     assert_eq!(mask, 0x000000ffffe00000);
/// ```
///
#[macro_export]
macro_rules! genmask {
    ($h:expr, $l:expr) => {{
        const _: () = {
            assert!($h >= $l);
        };
        ((!0u64 - (1u64 << $l) + 1) & (!0u64 >> (64 - 1 - $h)))
    }};
}
