// SPDX-License-Identifier: GPL-2.0

//! Framebuffer I/O helpers.
//!
//! This module provides safe wrappers for the C `fb_io_*` helpers.
//!
//! C header: [`include/linux/fb.h`](srctree/include/linux/fb.h)

use crate::{
    bindings,
    error::{to_result, Result},
    fb,
    fs::file,
    mm,
    prelude::*,
};

/// Generic framebuffer read helper.
///
/// Calls the C `fb_io_read` helper.
pub fn fb_io_read<T: fb::Driver>(
    device: &fb::Device<T>,
    buf: &mut [u8],
    ppos: &mut file::Offset,
) -> Result<usize> {
    // SAFETY: Both `device.as_raw()` and `ppos` are valid by type invariants, and `buf` is a valid
    // mutable slice. The C helper treats the buffer pointer as `__user` and will return `-EFAULT`
    // if it is not a valid user pointer.
    let result = unsafe {
        bindings::fb_io_read(
            device.as_raw(),
            buf.as_mut_ptr() as *mut core::ffi::c_char,
            buf.len(),
            ppos as *mut file::Offset as *mut bindings::loff_t,
        )
    };
    if result < 0 {
        Err(Error::from_errno(result as i32))
    } else {
        Ok(result as usize)
    }
}

/// Generic framebuffer write helper.
///
/// Calls the C `fb_io_write` helper.
pub fn fb_io_write<T: fb::Driver>(
    device: &fb::Device<T>,
    buf: &[u8],
    ppos: &mut file::Offset,
) -> Result<usize> {
    // SAFETY: Both `device.as_raw()` and `ppos` are valid by type invariants, and `buf` is a valid
    // slice. The C helper treats the buffer pointer as `__user` and will return `-EFAULT` if it is
    // not a valid user pointer.
    let result = unsafe {
        bindings::fb_io_write(
            device.as_raw(),
            buf.as_ptr() as *const core::ffi::c_char,
            buf.len(),
            ppos as *mut file::Offset as *mut bindings::loff_t,
        )
    };
    if result < 0 {
        Err(Error::from_errno(result as i32))
    } else {
        Ok(result as usize)
    }
}

/// Generic framebuffer mmap helper.
///
/// Calls the C `fb_io_mmap` helper.
pub fn fb_io_mmap<T: fb::Driver>(device: &fb::Device<T>, vma: &mm::virt::VmaNew) -> Result {
    // SAFETY: Both `device.as_raw()` and `vma.as_ptr()` are valid by type invariants.
    unsafe { to_result(bindings::fb_io_mmap(device.as_raw(), vma.as_ptr())) }
}
