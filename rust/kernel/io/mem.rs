// SPDX-License-Identifier: GPL-2.0

//! Generic memory-mapped IO.

use core::ops::Deref;

use crate::io::resource::Resource;
use crate::io::Io;
use crate::io::IoRaw;
use crate::prelude::*;

/// A generic memory-mapped IO region.
///
/// Accesses to the underlying region is checked either at compile time, if the
/// region's size is known at that point, or at runtime otherwise.
///
/// Whether `IoMem` represents an exclusive access to the underlying memory
/// region is determined by the caller at creation time, as overlapping access
/// may be needed in some cases.
///
/// # Invariants
///
/// `IoMem` always holds an `IoRaw` inststance that holds a valid pointer to the
/// start of the I/O memory mapped region and its size.
pub struct IoMem<const SIZE: usize = 0> {
    io: IoRaw<SIZE>,
    res_start: u64,
    exclusive: bool,
}

impl<const SIZE: usize> IoMem<SIZE> {
    /// Creates a new `IoMem` instance.
    ///
    /// `exclusive` determines whether the memory region should be exclusively
    ///
    /// # Safety
    ///
    /// The caller must ensure that the underlying resource remains valid
    /// throughout the `IoMem`'s lifetime. This is usually done by wrapping the
    /// `IoMem` in a `Devres` instance, which will properly revoke the access
    /// when the device is unbound from the matched driver.
    pub(crate) unsafe fn new(resource: &Resource, exclusive: bool) -> Result<Self> {
        let size = resource.size();
        if size == 0 {
            return Err(ENOMEM);
        }

        let res_start = resource.start();

        if exclusive {
            // SAFETY:
            // - `res_start` and `size` are read from a presumably valid `struct resource`.
            // - `size` is known not to be zero at this point.
            // - `resource.name()` returns a valid C string.
            let mem_region = unsafe {
                bindings::request_mem_region(res_start, size, resource.name().as_char_ptr())
            };

            if mem_region.is_null() {
                return Err(EBUSY);
            }
        }

        // SAFETY:
        // - `res_start` and `size` are read from a presumably valid `struct resource`.
        // - `size` is known not to be zero at this point.
        let addr = unsafe { bindings::ioremap(res_start, size as usize) };
        if addr.is_null() {
            if exclusive {
                // SAFETY:
                // - `res_start` and `size` are read from a presumably valid `struct resource`.
                // - `size` is the same as the one passed to `request_mem_region`.
                unsafe { bindings::release_mem_region(res_start, size) };
            }
            return Err(ENOMEM);
        }

        let io = IoRaw::new(addr as usize, size as usize)?;

        Ok(IoMem {
            io,
            res_start,
            exclusive,
        })
    }
}

impl<const SIZE: usize> Drop for IoMem<SIZE> {
    fn drop(&mut self) {
        if self.exclusive {
            // SAFETY: `res_start` and `io.maxsize()` were the values passed to
            // `request_mem_region`.
            unsafe { bindings::release_mem_region(self.res_start, self.io.maxsize() as u64) }
        }

        // SAFETY: Safe as by the invariant of `Io`.
        unsafe { bindings::iounmap(self.io.addr() as *mut core::ffi::c_void) }
    }
}

impl<const SIZE: usize> Deref for IoMem<SIZE> {
    type Target = Io<SIZE>;

    fn deref(&self) -> &Self::Target {
        // SAFETY: Safe as by the invariant of `IoMem`.
        unsafe { Io::from_raw(&self.io) }
    }
}
