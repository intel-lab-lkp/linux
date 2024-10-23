// SPDX-License-Identifier: GPL-2.0

//! Direct memory access (DMA).
//!
//! C header: [`include/linux/dma-mapping.h`](srctree/include/linux/dma-mapping.h)

use crate::{
    bindings,
    device::Device,
    error::code::*,
    error::Result,
    types::ARef,
};

/// Abstraction of dma_alloc_coherent
///
/// # Invariants
///
/// For the lifetime of an instance of CoherentAllocation:
/// 1. The cpu address pointer is valid and is accessed with an index bounded within count.
/// 2. We hold a reference to the device.
pub struct CoherentAllocation<T: 'static> {
    dev: ARef<Device>,
    dma_handle: bindings::dma_addr_t,
    count: usize,
    cpu_addr: &'static mut [T],
}

impl<T> CoherentAllocation<T> {
    /// Allocates a region of `size_of::<T> * count` of consistent memory.
    ///
    /// Returns a CoherentAllocation object which contains a pointer to the allocated region
    /// (in the processor's virtual address space) and the device address which can be
    /// given to the device as the DMA address base of the region. The region is released once
    /// [`CoherentAllocation`] is dropped.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::device::Device;
    /// use kernel::dma::CoherentAllocation;
    ///
    /// # fn dox(dev: &Device) -> Result<()> {
    /// let c: CoherentAllocation<u64> = CoherentAllocation::alloc_coherent(dev, 4, GFP_KERNEL)?;
    /// # Ok(()) }
    /// ```
    pub fn alloc_coherent(
        dev: &Device,
        count: usize,
        flags: kernel::alloc::Flags,
    ) -> Result<CoherentAllocation<T>> {
        let t_size = core::mem::size_of::<T>();
        let size = count.checked_mul(t_size).ok_or(EOVERFLOW)?;
        let mut dma_handle = 0;
        // SAFETY: device pointer is guaranteed as valid by invariant on `Device`.
        // We ensure that we catch the failure on this function and throw an ENOMEM
        let ret = unsafe {
            bindings::dma_alloc_attrs(
                dev.as_raw(),
                size,
                &mut dma_handle, flags.as_raw(),
                0,
            )
        };
        if ret.is_null() {
            return Err(ENOMEM)
        }

        Ok(Self {
            dev: dev.into(),
            dma_handle,
            count,
            // SAFETY: The raw buffer and size is valid from the checks we've made above.
            cpu_addr: unsafe { core::slice::from_raw_parts_mut(ret as _, size) },
        })
    }

    /// Reads a value on a location specified by index.
    pub fn read(&self, index: usize) -> Result<T>
    where
        T: Copy
    {
        if let Some(val) = self.cpu_addr.get(index) {
            Ok(*val)
        } else {
            Err(EINVAL)
        }
    }

    /// Write a value on the memory location specified by index.
    pub fn write(&mut self, index: usize, value: &T) -> Result
    where
        T: Copy,
    {
        if let Some(elem) = self.cpu_addr.get_mut(index) {
            *elem = *value;
            Ok(())
        } else {
            Err(EINVAL)
        }
    }

    /// Performs a read and then a write of a value on a location specified by index.
    pub fn read_write(&mut self, index: usize, value: &T) -> Result<T>
    where
        T: Copy,
    {
        if let Some(elem) = self.cpu_addr.get_mut(index) {
            let val = *elem;
            *elem = *value;
            Ok(val)
        } else {
            Err(EINVAL)
        }
    }

    /// Returns the base address to the allocated region and the dma handle.
    /// Caller takes ownership of returned resources.
    pub fn into_parts(self) -> (usize, bindings::dma_addr_t) {
        let ret = (self.cpu_addr.as_mut_ptr() as _, self.dma_handle);
        core::mem::forget(self);
        ret
    }

    /// Returns the base address to the allocated region in the CPU's virtual address space.
    pub fn start_ptr(&self) -> *const T {
        self.cpu_addr.as_ptr() as _
    }

    /// Returns the base address to the allocated region in the CPU's virtual address space as
    /// a mutable pointer.
    pub fn start_ptr_mut(&mut self) -> *mut T {
        self.cpu_addr.as_mut_ptr() as _
    }

    /// Returns a DMA handle which may given to the device as the DMA address base of
    /// the region.
    pub fn dma_handle(&self) -> bindings::dma_addr_t {
        self.dma_handle
    }
}

impl<T> Drop for CoherentAllocation<T> {
    fn drop(&mut self) {
        let size = self.count * core::mem::size_of::<T>();

        // SAFETY: the device, cpu address, and the dma handle is valid due to the
        // type invariants on `CoherentAllocation`.
        unsafe { bindings::dma_free_attrs(self.dev.as_raw(), size,
                                          self.cpu_addr.as_mut_ptr() as _,
                                          self.dma_handle, 0) }
    }
}
