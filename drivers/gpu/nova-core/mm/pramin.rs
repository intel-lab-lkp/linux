// SPDX-License-Identifier: GPL-2.0

//! Direct VRAM access through the PRAMIN aperture.
//!
//! PRAMIN provides a 1MB sliding window into VRAM through BAR0, allowing the CPU to access
//! video memory directly. The [`Window`] type automatically repositions the window when
//! accessing different VRAM regions and restores the original position on drop. This allows
//! to reuse the same window for multiple accesses in the same window.
//!
//! The PRAMIN aperture is a 1MB region at BAR0 + 0x700000 for all GPUs. The window base is
//! controlled by the `NV_PBUS_BAR0_WINDOW` register and must be 64KB aligned.
//!
//! # Examples
//!
//! ## Basic read/write
//!
//! ```no_run
//! use crate::driver::Bar0;
//! use crate::mm::pramin;
//!
//! fn example(bar: &Bar0) -> Result<()> {
//!     let mut pram_win = pramin::Window::new(bar);
//!
//!     // Write and read back.
//!     pram_win.try_write32(0x100, 0xDEADBEEF)?;
//!     let val = pram_win.try_read32(0x100)?;
//!     assert_eq!(val, 0xDEADBEEF);
//!
//!     Ok(())
//!     // Original window position restored on drop.
//! }
//! ```
//!
//! ## Auto-repositioning across VRAM regions
//!
//! ```no_run
//! use crate::driver::Bar0;
//! use crate::mm::pramin;
//!
//! fn example(bar: &Bar0) -> Result<()> {
//!     let mut pram_win = pramin::Window::new(bar);
//!
//!     // Access first 1MB region.
//!     pram_win.try_write32(0x100, 0x11111111)?;
//!
//!     // Access at 2MB - window auto-repositions.
//!     pram_win.try_write32(0x200000, 0x22222222)?;
//!
//!     // Back to first region - window repositions again.
//!     let val = pram_win.try_read32(0x100)?;
//!     assert_eq!(val, 0x11111111);
//!
//!     Ok(())
//! }
//! ```

#![allow(unused)]

use crate::{
    driver::Bar0,
    regs, //
};

use kernel::bits::genmask_u64;
use kernel::prelude::*;
use kernel::ptr::{Alignable, Alignment};
use kernel::sizes::{SZ_1M, SZ_64K};

/// PRAMIN aperture base offset in BAR0.
const PRAMIN_BASE: usize = 0x700000;

/// PRAMIN aperture size (1MB).
const PRAMIN_SIZE: usize = SZ_1M;

/// 64KB alignment for window base.
const WINDOW_ALIGN: Alignment = Alignment::new::<SZ_64K>();

/// Maximum addressable VRAM offset (40-bit address space).
///
/// The `NV_PBUS_BAR0_WINDOW` register has a 24-bit `window_base` field (bits 23:0) that stores
/// bits [39:16] of the target VRAM address. This limits the addressable space to 2^40 bytes.
const MAX_VRAM_OFFSET: usize = genmask_u64(0..=39) as usize;

/// Generate a PRAMIN read accessor.
macro_rules! define_pramin_read {
    ($name:ident, $ty:ty) => {
        #[doc = concat!("Read a `", stringify!($ty), "` from VRAM at the given offset.")]
        pub(crate) fn $name(&mut self, vram_offset: usize) -> Result<$ty> {
            let bar_offset = self.ensure_window(vram_offset, ::core::mem::size_of::<$ty>())?;
            self.bar.$name(bar_offset)
        }
    };
}

/// Generate a PRAMIN write accessor.
macro_rules! define_pramin_write {
    ($name:ident, $ty:ty) => {
        #[doc = concat!("Write a `", stringify!($ty), "` to VRAM at the given offset.")]
        pub(crate) fn $name(&mut self, vram_offset: usize, value: $ty) -> Result {
            let bar_offset = self.ensure_window(vram_offset, ::core::mem::size_of::<$ty>())?;
            self.bar.$name(value, bar_offset)
        }
    };
}

/// PRAMIN window for direct VRAM access.
///
/// The window auto-repositions when accessing VRAM offsets outside the current 1MB range.
/// Original window position is saved on creation and restored on drop.
pub(crate) struct Window<'a> {
    bar: &'a Bar0,
    saved_base: usize,
    current_base: usize,
}

impl<'a> Window<'a> {
    /// Create a new PRAMIN window accessor.
    ///
    /// Saves the current window position for restoration on drop.
    pub(crate) fn new(bar: &'a Bar0) -> Self {
        let saved_base = Self::read_window_base(bar);

        Self {
            bar,
            saved_base,
            current_base: saved_base,
        }
    }

    /// Read the current window base from the BAR0_WINDOW register.
    fn read_window_base(bar: &Bar0) -> usize {
        let reg = regs::NV_PBUS_BAR0_WINDOW::read(bar);
        // CAST: u32 to usize is lossless.
        (reg.window_base() as usize) << 16
    }

    /// Write a new window base to the BAR0_WINDOW register.
    fn write_window_base(bar: &Bar0, base: usize) {
        // CAST:
        // - We have guaranteed that the base is within the addressable range (40-bits).
        // - After >> 16, a 40-bit aligned base becomes 24 bits, which fits in u32.
        regs::NV_PBUS_BAR0_WINDOW::default()
            .set_window_base((base >> 16) as u32)
            .write(bar);
    }

    /// Ensure the window covers the given VRAM offset returning the BAR0 offset to use.
    fn ensure_window(&mut self, vram_offset: usize, access_size: usize) -> Result<usize> {
        // Validate VRAM offset is within addressable range (40-bit address space).
        let end_offset = vram_offset.checked_add(access_size).ok_or(EINVAL)?;
        if end_offset > MAX_VRAM_OFFSET + 1 {
            return Err(EINVAL);
        }

        // Calculate which 64KB-aligned base we need.
        let needed_base = vram_offset.align_down(WINDOW_ALIGN);

        // Calculate offset within the window.
        let offset_in_window = vram_offset - needed_base;

        // Check if access fits in 1MB window from this base.
        if offset_in_window + access_size > PRAMIN_SIZE {
            return Err(EINVAL);
        }

        // Reposition window if needed.
        if self.current_base != needed_base {
            Self::write_window_base(self.bar, needed_base);
            self.current_base = needed_base;
        }

        // Return BAR0 offset to access.
        Ok(PRAMIN_BASE + offset_in_window)
    }

    define_pramin_read!(try_read8, u8);
    define_pramin_read!(try_read16, u16);
    define_pramin_read!(try_read32, u32);
    define_pramin_read!(try_read64, u64);

    define_pramin_write!(try_write8, u8);
    define_pramin_write!(try_write16, u16);
    define_pramin_write!(try_write32, u32);
    define_pramin_write!(try_write64, u64);
}

impl Drop for Window<'_> {
    fn drop(&mut self) {
        // Restore the original window base if it changed.
        if self.current_base != self.saved_base {
            Self::write_window_base(self.bar, self.saved_base);
        }
    }
}

// SAFETY: `Window` requires `&mut self` for all accessors.
unsafe impl Send for Window<'_> {}

// SAFETY: `Window` requires `&mut self` for all accessors.
unsafe impl Sync for Window<'_> {}

/// Run PRAMIN self-tests during probe.
#[cfg(CONFIG_NOVA_PRAMIN_SELFTESTS)]
pub(crate) fn run_self_test(dev: &kernel::device::Device, bar: &Bar0) -> Result {
    dev_info!(dev, "PRAMIN: Starting self-test...\n");

    let mut win = Window::new(bar);

    // Use offset 0x1000 as test area.
    let base: usize = 0x1000;

    // Test 1: Read/write at odd-aligned locations.
    dev_info!(dev, "PRAMIN: Test 1 - Odd-aligned u8 read/write\n");
    for i in 0u8..4 {
        let offset = base + 1 + i as usize; // Offsets 0x1001, 0x1002, 0x1003, 0x1004
        let val = 0xA0 + i;
        win.try_write8(offset, val)?;
        let read_val = win.try_read8(offset)?;
        if read_val != val {
            dev_err!(
                dev,
                "PRAMIN: FAIL - offset {:#x}: wrote {:#x}, read {:#x}\n",
                offset,
                val,
                read_val
            );
            return Err(EIO);
        }
    }
    dev_info!(dev, "PRAMIN: Test 1 PASSED\n");

    // Test 2: Write u32 and read back as u8s.
    dev_info!(dev, "PRAMIN: Test 2 - Write u32, read as u8s\n");
    let test2_offset = base + 0x10;
    let test2_val: u32 = 0xDEADBEEF;
    win.try_write32(test2_offset, test2_val)?;

    // Read back as individual bytes (little-endian: EF BE AD DE).
    let expected_bytes: [u8; 4] = [0xEF, 0xBE, 0xAD, 0xDE];
    for (i, &expected) in expected_bytes.iter().enumerate() {
        let read_val = win.try_read8(test2_offset + i)?;
        if read_val != expected {
            dev_err!(
                dev,
                "PRAMIN: FAIL - offset {:#x}: expected {:#x}, read {:#x}\n",
                test2_offset + i,
                expected,
                read_val
            );
            return Err(EIO);
        }
    }
    dev_info!(dev, "PRAMIN: Test 2 PASSED\n");

    // Test 3: Window repositioning across 1MB boundaries.
    // Write to offset > 1MB to trigger window slide, then verify.
    dev_info!(dev, "PRAMIN: Test 4 - Window repositioning\n");
    let test4_offset_a = base; // First 1MB region
    let test4_offset_b = 0x200000 + base; // 2MB + base (different 1MB region)
    let val_a: u32 = 0x11111111;
    let val_b: u32 = 0x22222222;

    // Write to first region.
    win.try_write32(test4_offset_a, val_a)?;

    // Write to second region (triggers window reposition).
    win.try_write32(test4_offset_b, val_b)?;

    // Read back from second region.
    let read_b = win.try_read32(test4_offset_b)?;
    if read_b != val_b {
        dev_err!(
            dev,
            "PRAMIN: FAIL - offset {:#x}: expected {:#x}, read {:#x}\n",
            test4_offset_b,
            val_b,
            read_b
        );
        return Err(EIO);
    }

    // Read back from first region (triggers window reposition again).
    let read_a = win.try_read32(test4_offset_a)?;
    if read_a != val_a {
        dev_err!(
            dev,
            "PRAMIN: FAIL - offset {:#x}: expected {:#x}, read {:#x}\n",
            test4_offset_a,
            val_a,
            read_a
        );
        return Err(EIO);
    }
    dev_info!(dev, "PRAMIN: Test 3 PASSED\n");

    dev_info!(dev, "PRAMIN: All self-tests PASSED\n");
    Ok(())
}
