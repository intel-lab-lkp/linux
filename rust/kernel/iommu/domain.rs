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
    error::from_err_ptr,
    prelude::*, //
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
pub struct Domain<'a> {
    ptr: NonNull<bindings::iommu_domain>,
    _dev: PhantomData<&'a Device<Bound>>,
}

// SAFETY: A `struct iommu_domain` is not tied to the thread that allocated it.
unsafe impl<'a> Send for Domain<'a> {}

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
}

impl<'a> Drop for Domain<'a> {
    fn drop(&mut self) {
        // SAFETY: By the type invariant, `self.ptr` is a domain owned by `self`.
        unsafe { bindings::iommu_domain_free(self.ptr.as_ptr()) };
    }
}
