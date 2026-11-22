// SPDX-License-Identifier: GPL-2.0 or MIT
//! GEM buffer object management for the Tyr driver.
//!
//! This module provides buffer object (BO) management functionality using
//! DRM's GEM subsystem with shmem backing.

use core::ops::Range;

use kernel::{
    device::{
        Bound,
        Device, //
    },
    drm::gem::{
        self,
        shmem,
        BaseObject, //
    },
    page::PAGE_SIZE,
    prelude::*,
    sync::{
        aref::ARef,
        Arc, //
    },
    uapi, //
};

use crate::{
    driver::{
        TyrDrmDevice,
        TyrDrmDriver, //
    },
    file::TyrDrmFile,
    vm::{
        Vm,
        VmMapFlags, //
    },
};

/// Tyr's DriverObject type for GEM objects.
#[pin_data]
pub(crate) struct BoData {
    flags: u32,
}

impl BoData {
    /// Returns the flags the BO was created with.
    #[expect(dead_code)]
    pub(crate) fn flags(&self) -> u32 {
        self.flags
    }
}

/// Provides a way to pass arguments when creating BoData
/// as required by the gem::DriverObject trait.
pub(crate) struct BoCreateArgs {
    flags: u32,
}

impl gem::DriverObject for BoData {
    type Driver = TyrDrmDriver;
    type Args = BoCreateArgs;

    fn new(_dev: &TyrDrmDevice, _size: usize, args: BoCreateArgs) -> impl PinInit<Self, Error> {
        try_pin_init!(Self { flags: args.flags })
    }
}

/// Type alias for Tyr GEM buffer objects.
pub(crate) type Bo = gem::shmem::Object<BoData>;

/// Returns whether a BO should be mapped write-combine.
pub(crate) fn should_map_wc(coherent: bool, flags: u32) -> bool {
    if coherent {
        return false;
    }

    if flags & uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_WB_MMAP != 0 {
        return false;
    }

    true
}

/// Create a new GEM buffer object.
#[expect(dead_code)]
pub(crate) fn new_object(
    ddev: &TyrDrmDevice,
    pdev: &Device<Bound>,
    size: usize,
    flags: u32,
    coherent: bool,
) -> Result<ARef<Bo>> {
    if size == 0 {
        return Err(EINVAL);
    }

    let aligned_size = size.checked_next_multiple_of(PAGE_SIZE).ok_or(EINVAL)?;

    let map_wc = should_map_wc(coherent, flags);
    let bo = Bo::new(
        ddev,
        aligned_size,
        shmem::ObjectConfig {
            map_wc,
            parent_resv_obj: None,
        },
        BoCreateArgs { flags },
    )?;

    bo.sg_table(pdev)?;

    Ok(bo)
}

/// Look up a GEM object by handle for a DRM file.
pub(crate) fn lookup_handle(file: &TyrDrmFile, handle: u32) -> Result<ARef<Bo>> {
    Bo::lookup_handle(file, handle)
}

/// Creates a dummy GEM object to serve as the root of a GPUVM.
pub(crate) fn new_dummy_object(ddev: &TyrDrmDevice, coherent: bool) -> Result<ARef<Bo>> {
    // FIXME: use a Rust resv-object abstraction once available, rather than a real BO.
    Bo::new(
        ddev,
        4096,
        shmem::ObjectConfig {
            map_wc: should_map_wc(coherent, 0),
            parent_resv_obj: None,
        },
        BoCreateArgs { flags: 0 },
    )
}

/// Specifies how to choose a GPU virtual address for a [`KernelBo`].
/// An automatic VA allocation strategy will be added in the future.
pub(crate) enum KernelBoVaAlloc {
    /// Explicit VA address specified by the caller.
    Explicit(u64),
}

/// A kernel-owned buffer object with automatic GPU virtual address mapping.
///
/// This structure represents a buffer object that is created and managed entirely
/// by the kernel driver, as opposed to userspace-created GEM objects. It combines
/// a GEM object with automatic GPU virtual address (VA) space mapping and cleanup.
///
/// When dropped, the buffer is automatically unmapped from the GPU VA space.
pub(crate) struct KernelBo<'drm> {
    /// The underlying GEM buffer object.
    bo: ARef<Bo>,
    /// The GPU VM this buffer is mapped into.
    vm: Arc<Vm<'drm>>,
    /// The GPU VA range occupied by this buffer.
    va_range: Range<u64>,
}

impl<'drm> KernelBo<'drm> {
    /// Creates a new kernel-owned buffer object and maps it into GPU VA space.
    ///
    /// This function allocates a new shmem-backed GEM object and immediately maps
    /// it into the specified GPU virtual memory space. The mapping is automatically
    /// cleaned up when the [`KernelBo`] is dropped.
    pub(crate) fn new(
        ddev: &TyrDrmDevice,
        vm: Arc<Vm<'drm>>,
        size: u64,
        va_alloc: KernelBoVaAlloc,
        flags: VmMapFlags,
        coherent: bool,
    ) -> Result<Self> {
        if size == 0 {
            dev_err!(vm.dev(), "Cannot create KernelBo with size 0");
            return Err(EINVAL);
        }

        let KernelBoVaAlloc::Explicit(va) = va_alloc;

        let bo_size = usize::try_from(size).map_err(|_| EOVERFLOW)?;
        let va_end = va.checked_add(size).ok_or(EINVAL)?;

        let bo = Bo::new(
            ddev,
            bo_size,
            shmem::ObjectConfig {
                map_wc: should_map_wc(coherent, 0),
                parent_resv_obj: None,
            },
            BoCreateArgs { flags: 0 },
        )?;

        vm.map_bo_range(&bo, 0, size, va, flags)?;

        Ok(KernelBo {
            bo,
            vm,
            va_range: va..va_end,
        })
    }

    pub(crate) fn bo(&self) -> &Bo {
        &self.bo
    }
}

impl Drop for KernelBo<'_> {
    fn drop(&mut self) {
        let va = self.va_range.start;
        let size = self.va_range.end - self.va_range.start;

        if let Err(e) = self.vm.unmap_range(va, size) {
            // If unmap_range fails, it is still safe to drop the
            // KernelBo and its ARef to the GEM buffer object because
            // GPUVM also holds a reference to the GEM buffer object.
            // The physical pages won't be freed or reallocated.
            dev_err!(
                self.vm.dev(),
                "Failed to unmap KernelBo range {:#x}..{:#x}: {:?}",
                self.va_range.start,
                self.va_range.end,
                e
            );
        }
    }
}
