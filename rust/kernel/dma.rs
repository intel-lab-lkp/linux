// SPDX-License-Identifier: GPL-2.0

//! Direct memory access (DMA).
//!
//! C header: [`include/linux/dma-mapping.h`](srctree/include/linux/dma-mapping.h)

use crate::{
    bindings,
    build_assert,
    device::Device,
    error::code::*,
    error::Result,
    types::ARef,
    transmute::{AsBytes, FromBytes},
};

/// An abstraction of the `dma_alloc_coherent` API.
///
/// This is an abstraction around the `dma_alloc_coherent` API which is used to allocate and map
/// large consistent DMA regions.
///
/// A [`CoherentAllocation`] instance contains a pointer to the allocated region (in the
/// processor's virtual address space) and the device address which can be given to the device
/// as the DMA address base of the region. The region is released once [`CoherentAllocation`]
/// is dropped.
///
/// # Invariants
///
/// For the lifetime of an instance of [`CoherentAllocation`], the cpu address is a valid pointer
/// to an allocated region of consistent memory and we hold a reference to the device.
pub struct CoherentAllocation<T: AsBytes + FromBytes> {
    dev: ARef<Device>,
    dma_handle: bindings::dma_addr_t,
    count: usize,
    cpu_addr: *mut T,
}

impl<T: AsBytes + FromBytes> CoherentAllocation<T> {
    /// Allocates a region of `size_of::<T> * count` of consistent memory.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::device::Device;
    /// use kernel::dma::CoherentAllocation;
    ///
    /// # fn test(dev: &Device) -> Result {
    /// let c: CoherentAllocation<u64> = CoherentAllocation::alloc_coherent(dev, 4, GFP_KERNEL)?;
    /// # Ok::<(), Error>(()) }
    /// ```
    pub fn alloc_coherent(
        dev: &Device,
        count: usize,
        flags: kernel::alloc::Flags,
    ) -> Result<CoherentAllocation<T>> {
        build_assert!(core::mem::size_of::<T>() > 0,
                      "It doesn't make sense for the allocated type to be a ZST");

        let size = count.checked_mul(core::mem::size_of::<T>()).ok_or(EOVERFLOW)?;
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
        // INVARIANT: We just successfully allocated a coherent region which is accessible for
        // `count` elements, hence the cpu address is valid. We also hold a refcounted reference
        // to the device.
        Ok(Self {
            dev: dev.into(),
            dma_handle,
            count,
            cpu_addr: ret as *mut T,
        })
    }

    /// Returns the base address to the allocated region and the dma handle. The caller takes
    /// ownership of the returned resources.
    pub fn into_parts(self) -> (usize, bindings::dma_addr_t) {
        let ret = (self.cpu_addr as _, self.dma_handle);
        core::mem::forget(self);
        ret
    }

    /// Returns the base address to the allocated region in the CPU's virtual address space.
    pub fn start_ptr(&self) -> *const T {
        self.cpu_addr as _
    }

    /// Returns the base address to the allocated region in the CPU's virtual address space as
    /// a mutable pointer.
    pub fn start_ptr_mut(&mut self) -> *mut T {
        self.cpu_addr
    }

    /// Returns a DMA handle which may given to the device as the DMA address base of
    /// the region.
    pub fn dma_handle(&self) -> bindings::dma_addr_t {
        self.dma_handle
    }

    /// Returns the CPU-addressable region as a slice.
    pub fn cpu_buf(&self) -> &[T]
    {
        // SAFETY: The pointer is valid due to type invariant on `CoherentAllocation` and
        // is valid for reads for `self.count * size_of::<T>` bytes.
        unsafe { core::slice::from_raw_parts(self.cpu_addr, self.count) }
    }

    /// Performs the same functionality as `cpu_buf`, except that a mutable slice is returned.
    pub fn cpu_buf_mut(&mut self) -> &mut [T]
    {
        // SAFETY: The pointer is valid due to type invariant on `CoherentAllocation` and
        // is valid for reads for `self.count * size_of::<T>` bytes.
        unsafe { core::slice::from_raw_parts_mut(self.cpu_addr, self.count) }
    }
}

impl<T: AsBytes + FromBytes> Drop for CoherentAllocation<T> {
    fn drop(&mut self) {
        let size = self.count * core::mem::size_of::<T>();
        // SAFETY: the device, cpu address, and the dma handle is valid due to the
        // type invariants on `CoherentAllocation`.
        unsafe { bindings::dma_free_attrs(self.dev.as_raw(), size,
                                          self.cpu_addr as _,
                                          self.dma_handle, 0) }
    }
}
