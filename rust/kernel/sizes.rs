// SPDX-License-Identifier: GPL-2.0

//! Commonly used sizes.
//!
//! C headers: [`include/linux/sizes.h`](srctree/include/linux/sizes.h).
//!
//! The top-level `SZ_*` constants are [`usize`]-typed, for use in kernel page
//! arithmetic and similar CPU-side work.
//!
//! The [`DeviceSize`] trait provides the same constants as associated constants
//! on [`u32`] and [`u64`], for use in device address spaces where the address
//! width depends on the hardware. Device drivers frequently need these constants
//! as [`u64`] (or [`u32`]) rather than [`usize`], because device address spaces
//! are sized independently of the CPU pointer width.
//!
//! ```
//! use kernel::sizes::{DeviceSize, SZ_1M};
//!
//! // usize constant (CPU-side)
//! let pages: usize = SZ_1M / kernel::page::PAGE_SIZE;
//!
//! // Device-side constant via the trait
//! let heap_size: u64 = 14 * u64::SZ_1M;
//! let small: u32 = u32::SZ_4K;
//! ```

macro_rules! define_sizes {
    ($($name:ident),* $(,)?) => {
        // `usize` constants, from the C `SZ_*` defines in `include/linux/sizes.h`.
        $(
            #[doc = concat!("`", stringify!($name), "` as a [`usize`].")]
            pub const $name: usize = bindings::$name as usize;
        )*

        /// Size constants for device address spaces.
        ///
        /// Implemented for [`u32`] and [`u64`] so drivers can choose the width
        /// that matches their hardware. All `SZ_*` values fit in a [`u32`], so
        /// both implementations are lossless.
        ///
        /// ```
        /// use kernel::sizes::DeviceSize;
        ///
        /// let gpu_heap: u64 = 14 * u64::SZ_1M;
        /// let mmio_window: u32 = u32::SZ_16M;
        /// ```
        pub trait DeviceSize {
            $(
                #[doc = concat!("`", stringify!($name), "` for this type.")]
                const $name: Self;
            )*
        }

        impl DeviceSize for u32 {
            $(
                const $name: Self = {
                    assert!(self::$name <= u32::MAX as usize);
                    self::$name as u32
                };
            )*
        }

        impl DeviceSize for u64 {
            $(
                const $name: Self = self::$name as u64;
            )*
        }
    };
}

define_sizes! {
    SZ_1K,   // 0x0000_0400
    SZ_2K,   // 0x0000_0800
    SZ_4K,   // 0x0000_1000
    SZ_8K,   // 0x0000_2000
    SZ_16K,  // 0x0000_4000
    SZ_32K,  // 0x0000_8000
    SZ_64K,  // 0x0001_0000
    SZ_128K, // 0x0002_0000
    SZ_256K, // 0x0004_0000
    SZ_512K, // 0x0008_0000
    SZ_1M,   // 0x0010_0000
    SZ_2M,   // 0x0020_0000
    SZ_4M,   // 0x0040_0000
    SZ_8M,   // 0x0080_0000
    SZ_16M,  // 0x0100_0000
    SZ_32M,  // 0x0200_0000
    SZ_64M,  // 0x0400_0000
    SZ_128M, // 0x0800_0000
    SZ_256M, // 0x1000_0000
    SZ_512M, // 0x2000_0000
    SZ_1G,   // 0x4000_0000
    SZ_2G,   // 0x8000_0000
}
