// SPDX-License-Identifier: GPL-2.0

//! IOMMU paging domains.
//!
//! An IOMMU domain is an address space: the devices attached to it share one translation from
//! I/O virtual addresses (IOVAs) to physical addresses. In a paging domain, that translation
//! consists of ranges that are mapped with `iommu_map()` and unmapped with `iommu_unmap()`.
//!
//! C header: [`include/linux/iommu.h`](srctree/include/linux/iommu.h)

use core::{
    marker::PhantomData,
    ptr::NonNull, //
};

use crate::{
    bindings,
    device::{
        Bound,
        Device, //
    },
    error::{
        from_err_ptr,
        to_result, //
    },
    prelude::*,
    scatterlist::{
        Owned,
        SGTable, //
    }, //
};

/// An IOMMU paging domain.
///
/// An IOMMU domain is an address space: the devices attached to it share one translation from
/// I/O virtual addresses (IOVAs) to physical addresses. In a paging domain, that translation
/// consists of ranges that are mapped with `iommu_map()` and unmapped with `iommu_unmap()`.
///
/// The domain is allocated for a bound device and cannot outlive that device's binding. It is
/// freed when dropped, which may sleep.
///
/// # Invariants
///
/// `ptr` points to a paging domain allocated by `iommu_paging_domain_alloc_flags()` and owned by
/// this object.
///
/// # Examples
///
/// Two clients share one device. Each has its own domain, and the device is switched from one
/// to the other between jobs.
///
/// ```
/// use kernel::{
///     device::{
///         Bound,
///         Device, //
///     },
///     dma,
///     iommu::{
///         domain::{
///             Domain,
///             MapSource, //
///         },
///         prot, //
///     },
///     page,
///     prelude::*,
///     scatterlist::SGTable, //
/// };
///
/// fn new_buffer(dev: &Device<Bound>) -> Result<impl MapSource + 'static> {
///     let pages = VVec::<u8>::with_capacity(4 * page::PAGE_SIZE, GFP_KERNEL)?;
///
///     KBox::pin_init(
///         SGTable::new(dev, pages, dma::DataDirection::Bidirectional, GFP_KERNEL),
///         GFP_KERNEL,
///     )
/// }
///
/// fn test(dev: &Device<Bound>) -> Result {
///     // SAFETY: The IOMMU driver that `dev` is behind meets the requirements of `Domain::new()`.
///     let domain_a = unsafe { Domain::new(dev)? };
///     // SAFETY: The IOMMU driver that `dev` is behind meets the requirements of `Domain::new()`.
///     let domain_b = unsafe { Domain::new(dev)? };
///
///     // Both clients map their buffer at the same IOVA.
///     let iova = 0xdead_0000;
///
///     // SAFETY: We are the driver bound to `dev`. The attachment is dropped below, after `dev`
///     // has stopped.
///     let attachment = unsafe { domain_a.attach(dev)? };
///
///     let buffer = new_buffer(dev)?;
///     // SAFETY: `domain_a` has no other mapping, and nothing else uses it.
///     let _mapping_a = unsafe { domain_a.map(iova, buffer, prot::READ | prot::WRITE)? };
///
///     // Run a job of the first client, then stop the device.
///
///     // `attach()` allows one domain on a device at a time, so `domain_a` is detached first.
///     drop(attachment);
///
///     // SAFETY: We are the driver bound to `dev`. The attachment is dropped when this function
///     // returns, after `dev` has stopped.
///     let _attachment = unsafe { domain_b.attach(dev)? };
///
///     let buffer = new_buffer(dev)?;
///     // SAFETY: `domain_b` has no other mapping, and nothing else uses it.
///     let _mapping_b = unsafe { domain_b.map(iova, buffer, prot::READ | prot::WRITE)? };
///
///     // Run a job of the second client, then stop the device. With `domain_b` attached, `iova`
///     // leads to the second buffer.
///
///     Ok(())
/// }
/// ```
pub struct Domain<'a> {
    ptr: NonNull<bindings::iommu_domain>,
    _dev: PhantomData<&'a Device<Bound>>,
}

// SAFETY: A `struct iommu_domain` is not tied to the thread that allocated it.
unsafe impl<'a> Send for Domain<'a> {}

// SAFETY: `&Domain` allows the following from several threads at once:
// - Attaching and detaching the domain. `iommu_attach_device()` and `iommu_detach_device()` may
//   be called concurrently for the same domain.
// - Mapping and unmapping ranges. `iommu_map_sg()` and `iommu_unmap()` may be called concurrently
//   for the same domain, except in the cases that the safety requirements of `map()` exclude.
unsafe impl<'a> Sync for Domain<'a> {}

impl<'a> Domain<'a> {
    /// Allocates a paging domain for `dev`.
    ///
    /// # Safety
    ///
    /// The IOMMU driver that `dev` is behind must:
    ///
    /// - Set a domain up fully when the domain is allocated, and keep it and its mappings until
    ///   the domain is freed, whether or not a device is attached.
    /// - Allow one domain to be attached, detached, mapped and unmapped from several threads at
    ///   once.
    pub unsafe fn new(dev: &'a Device<Bound>) -> Result<Self> {
        // SAFETY: `dev` is a valid device, bound for `'a`.
        let ptr =
            from_err_ptr(unsafe { bindings::iommu_paging_domain_alloc_flags(dev.as_raw(), 0) })?;

        // SAFETY: `iommu_paging_domain_alloc_flags()` never returns NULL.
        let ptr = unsafe { NonNull::new_unchecked(ptr) };

        // INVARIANT: `ptr` was allocated by `iommu_paging_domain_alloc_flags()` and is owned by
        // the returned object.
        Ok(Self {
            ptr,
            _dev: PhantomData,
        })
    }

    /// Attaches the domain to `dev`.
    ///
    /// This may sleep.
    ///
    /// # Safety
    ///
    /// - The caller must be the driver bound to `dev`.
    /// - The caller must ensure that `dev` stops using the domain before the returned
    ///   [`Attachment`] is dropped.
    /// - The caller must not `mem::forget()` the returned [`Attachment`] or otherwise prevent its
    ///   [`Drop`] implementation from running.
    pub unsafe fn attach<'d>(&'d self, dev: &'d Device<Bound>) -> Result<Attachment<'d>> {
        // SAFETY: By the type invariant, `self.ptr` is a valid domain, and `dev` is a valid
        // device. The caller is the driver bound to `dev`.
        to_result(unsafe { bindings::iommu_attach_device(self.ptr.as_ptr(), dev.as_raw()) })?;

        // INVARIANT: The domain is attached to `dev`.
        Ok(Attachment { domain: self, dev })
    }

    /// Maps the pages of `source` at `iova` with the protection flags `prot` (see [`prot`]).
    ///
    /// The returned [`Mapping`] owns `source` and unmaps the range when it is dropped. A leaked
    /// [`Mapping`] leaves its range mapped. Its source is leaked with it and must keep the pages
    /// allocated, so `S` cannot borrow them and has to be `'static`.
    ///
    /// This performs no cache coherency operations on the pages. It may sleep.
    ///
    /// [`prot`]: super::prot
    ///
    /// # Safety
    ///
    /// - The range must not overlap any other mapping in the domain.
    /// - No other operation on the domain may access the range while this function runs.
    pub unsafe fn map<'d, S: MapSource + 'static>(
        &'d self,
        iova: usize,
        source: S,
        prot: u32,
    ) -> Result<Mapping<'d, S>> {
        let sgt = source.sg_table().as_raw();

        // SAFETY: `sgt` points to the table of `source`, which is valid.
        let (sgl, nents) = unsafe { ((*sgt).sgl, (*sgt).orig_nents) };

        // SAFETY:
        // - By the type invariant, `self.ptr` is a valid domain.
        // - `sgl` has `nents` entries. By the safety requirements of `MapSource`, they describe
        //   pages that stay allocated for as long as the returned `Mapping` owns `source`.
        // - The caller meets the safety requirements of this function.
        let ret = unsafe {
            bindings::iommu_map_sg(
                self.ptr.as_ptr(),
                iova,
                sgl,
                nents,
                prot as i32,
                GFP_KERNEL.as_raw(),
            )
        };
        if ret < 0 {
            return Err(Error::from_errno(ret as i32));
        }

        // INVARIANT: `iommu_map_sg()` mapped `[iova, iova + ret)`, and only the returned object
        // unmaps it.
        Ok(Mapping {
            domain: self,
            iova,
            size: ret as usize,
            _source: source,
        })
    }
}

impl<'a> Drop for Domain<'a> {
    fn drop(&mut self) {
        // SAFETY: By the type invariant, `self.ptr` is a domain owned by `self`. Every
        // `Attachment` borrows `self`, and the caller of `attach()` promised not to leak it, so
        // the domain is attached to no device.
        unsafe { bindings::iommu_domain_free(self.ptr.as_ptr()) };
    }
}

/// A [`Domain`] attached to a device.
///
/// An attachment cannot outlive the domain or the binding of the device it is attached to.
/// Dropping it detaches the domain from the device, which may sleep.
///
/// # Invariants
///
/// `domain` is attached to `dev`.
#[must_use = "the domain is detached when the attachment is dropped"]
pub struct Attachment<'d> {
    domain: &'d Domain<'d>,
    dev: &'d Device<Bound>,
}

impl<'d> Drop for Attachment<'d> {
    fn drop(&mut self) {
        // SAFETY: By the type invariant, `self.domain` is attached to `self.dev`. The caller of
        // `Domain::attach()` is the driver bound to `self.dev`, and promised that the device
        // stopped using the domain.
        unsafe { bindings::iommu_detach_device(self.domain.ptr.as_ptr(), self.dev.as_raw()) };
    }
}

/// A source of pages that can be mapped into a [`Domain`].
///
/// # Safety
///
/// Every entry of the table that [`MapSource::sg_table`] returns must describe pages. The table
/// must not change and its pages must stay allocated for as long as `self` exists.
pub unsafe trait MapSource {
    /// Returns the table describing the pages to map.
    fn sg_table(&self) -> &SGTable;
}

// SAFETY: An `SGTable<Owned<P>>` is built from pages, and keeps them and its table until it is
// dropped.
unsafe impl<P> MapSource for Pin<KBox<SGTable<Owned<P>>>> {
    fn sg_table(&self) -> &SGTable {
        self
    }
}

/// Pages mapped into a [`Domain`].
///
/// Dropping a mapping unmaps its range, then drops its source.
///
/// # Invariants
///
/// `[iova, iova + size)` is mapped in `domain`, and only this object unmaps it.
#[must_use = "the range is unmapped when the mapping is dropped"]
pub struct Mapping<'d, S> {
    domain: &'d Domain<'d>,
    iova: usize,
    size: usize,
    _source: S,
}

impl<'d, S> Mapping<'d, S> {
    /// Returns the first IOVA of the mapping.
    #[inline]
    pub fn iova(&self) -> usize {
        self.iova
    }
}

impl<'d, S> Drop for Mapping<'d, S> {
    fn drop(&mut self) {
        // SAFETY: By the type invariant, `[self.iova, self.iova + self.size)` is mapped in
        // `self.domain`, and nothing else unmaps it.
        unsafe { bindings::iommu_unmap(self.domain.ptr.as_ptr(), self.iova, self.size) };
    }
}
