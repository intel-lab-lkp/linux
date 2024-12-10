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

/// Possible attributes associated with a DMA mapping.
///
/// They can be combined with the operators `|`, `&`, and `!`.
///
/// Values can be used from the [`attrs`] module.
#[derive(Clone, Copy, PartialEq)]
pub struct Attribs(u32);

impl Attribs {
    /// Get the raw representation of this attribute.
    pub(crate) fn as_raw(self) -> u64 {
        self.0.into()
    }

    /// Check whether `flags` is contained in `self`.
    pub fn contains(self, flags: Attribs) -> bool {
        (self & flags) == flags
    }
}

impl core::ops::BitOr for Attribs {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Attribs {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for Attribs {
    type Output = Self;
    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

/// DMA mapping attrributes.
pub mod attrs {
    use super::Attribs;

    /// Specifies that reads and writes to the mapping may be weakly ordered, that is that reads
    /// and writes may pass each other.
    pub const DMA_ATTR_WEAK_ORDERING: Attribs = Attribs(bindings::DMA_ATTR_WEAK_ORDERING);

    /// Specifies that writes to the mapping may be buffered to improve performance.
    pub const DMA_ATTR_WRITE_COMBINE: Attribs = Attribs(bindings::DMA_ATTR_WRITE_COMBINE);

    /// Lets the platform to avoid creating a kernel virtual mapping for the allocated buffer.
    pub const DMA_ATTR_NO_KERNEL_MAPPING: Attribs = Attribs(bindings::DMA_ATTR_NO_KERNEL_MAPPING);

    /// Allows platform code to skip synchronization of the CPU cache for the given buffer assuming
    /// that it has been already transferred to 'device' domain.
    pub const DMA_ATTR_SKIP_CPU_SYNC: Attribs = Attribs(bindings::DMA_ATTR_SKIP_CPU_SYNC);

    /// Forces contiguous allocation of the buffer in physical memory.
    pub const DMA_ATTR_FORCE_CONTIGUOUS: Attribs = Attribs(bindings::DMA_ATTR_FORCE_CONTIGUOUS);

    /// This is a hint to the DMA-mapping subsystem that it's probably not worth the time to try
    /// to allocate memory to in a way that gives better TLB efficiency.
    pub const DMA_ATTR_ALLOC_SINGLE_PAGES: Attribs = Attribs(bindings::DMA_ATTR_ALLOC_SINGLE_PAGES);

    /// This tells the DMA-mapping subsystem to suppress allocation failure reports (similarly to
    /// __GFP_NOWARN).
    pub const DMA_ATTR_NO_WARN: Attribs = Attribs(bindings::DMA_ATTR_NO_WARN);

    /// Used to indicate that the buffer is fully accessible at an elevated privilege level (and
    /// ideally inaccessible or at least read-only at lesser-privileged levels).
    pub const DMA_ATTR_PRIVILEGED: Attribs = Attribs(bindings::DMA_ATTR_PRIVILEGED);
}

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
    dma_attrs: Attribs,
}

impl<T: AsBytes + FromBytes> CoherentAllocation<T> {
    /// Allocates a region of `size_of::<T> * count` of consistent memory.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::device::Device;
    /// use kernel::dma::{attrs::*, CoherentAllocation};
    ///
    /// # fn test(dev: &Device) -> Result {
    /// let c: CoherentAllocation<u64> = CoherentAllocation::alloc_attrs(dev, 4, GFP_KERNEL,
    ///                                                                  DMA_ATTR_NO_WARN)?;
    /// # Ok::<(), Error>(()) }
    /// ```
    pub fn alloc_attrs(
        dev: &Device,
        count: usize,
        gfp_flags: kernel::alloc::Flags,
        dma_attrs: Attribs,
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
                &mut dma_handle, gfp_flags.as_raw(),
                dma_attrs.as_raw(),
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
            dma_attrs,
        })
    }

    /// Performs the same functionality as `alloc_attrs`, except the `dma_attrs` is 0 by default.
    pub fn alloc_coherent(dev: &Device,
                          count: usize,
                          gfp_flags: kernel::alloc::Flags) -> Result<CoherentAllocation<T>> {
        CoherentAllocation::alloc_attrs(dev, count, gfp_flags, Attribs(0))
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
                                          self.dma_handle,
                                          self.dma_attrs.as_raw(),) }
    }
}
