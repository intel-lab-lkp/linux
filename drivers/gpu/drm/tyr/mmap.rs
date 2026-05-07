// SPDX-License-Identifier: GPL-2.0 or MIT

//! Memory-mapping support for the Tyr DRM driver.
//!
//! This module implements Tyr's DRM `mmap` file operation. Normal GEM buffer
//! mappings are handled by the standard DRM GEM mmap path
//! (`bindings::drm_gem_mmap`), while special Panthor userspace-MMIO mappings
//! are handled directly by Tyr.
//!
//! # Userspace MMIO mappings
//!
//! Panthor defines reserved DRM mmap offsets for exposing selected GPU MMIO
//! pages directly to userspace. These are special uAPI marker offsets, not
//! hardware register offsets.
//!
//! Currently the only supported userspace-MMIO mapping is the flush-ID register
//! page ([`DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET`]). This mapping exposes a
//! read-only, non-cached MMIO page that userspace can poll to observe GPU cache
//! flush completion without requiring a kernel round-trip.
//!
//! The VMA for this mapping is configured in [`mmap_user_mmio`]. Physical MMIO
//! pages are inserted lazily through [`VM_OPS`] and [`vm_fault_handler`] when
//! userspace first accesses the mapping.

// TODO: When runtime PM support is added, userspace-MMIO mappings must be
// integrated with suspend/resume. Accessing this MMIO page while the register
// bank clock is disabled can hang the device. Runtime suspend will need to invalidate
// existing userspace-MMIO mappings, and mmap/fault handling while suspended
// should expose a dummy zero page instead of the real MMIO PFN.

use kernel::{
    bindings,
    drm,
    error::code::*,
    io::register::Register,
    mm::virt::{
        flags as vma_flags,
        VmaNew, //
    },
    page::{
        PAGE_SHIFT,
        PAGE_SIZE, //
    },
    prelude::*, //
};

use crate::{
    driver::TyrDrmDevice,
    file::TyrDrmFileData,
    regs::user::LATEST_FLUSH, //
};

// Reserved Panthor mmap offsets for userspace-MMIO mappings.
//
// These offsets are special uAPI marker values used to distinguish
// userspace-MMIO mappings from normal GEM object mappings in the DRM mmap
// path. They must match uapi/drm/panthor_drm.h exactly.
//
// These are not GPU register offsets or physical MMIO addresses.
//
// Tyr currently supports only the 64-bit Panthor mmap offsets. Panthor also
// defines 32-bit compat offsets, but that path is not implemented yet.
const DRM_PANTHOR_USER_MMIO_OFFSET_64BIT: u64 = 1u64 << 56;
const DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET: u64 = DRM_PANTHOR_USER_MMIO_OFFSET_64BIT;

// `vm_pgoff` stores mmap offsets in PAGE_SIZE units rather than byte offsets,
// so convert the Panthor uAPI mmap marker offsets into page offsets before
// comparing them against `vm_pgoff`.
const DRM_PANTHOR_USER_MMIO_PGOFF_64BIT: u64 = DRM_PANTHOR_USER_MMIO_OFFSET_64BIT >> PAGE_SHIFT;
const DRM_PANTHOR_USER_FLUSH_ID_MMIO_PGOFF: u64 =
    DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET >> PAGE_SHIFT;

pub(crate) const FOPS: bindings::file_operations = {
    let mut fops: bindings::file_operations = drm::gem::create_fops();
    fops.mmap = Some(mmap_callback);
    fops
};

/// C-callable mmap file operation entry point for Tyr.
///
/// Handles the special Panthor flush-ID mmap offset directly and falls back to
/// [`bindings::drm_gem_mmap`] for normal GEM object mappings.
///
/// # Safety
///
/// `filp` and `vma` must be valid pointers provided by the kernel mmap path.
unsafe extern "C" fn mmap_callback(
    filp: *mut bindings::file,
    vma: *mut bindings::vm_area_struct,
) -> core::ffi::c_int {
    // SAFETY: `filp` is a valid DRM file provided by the mmap callback. DRM open
    // initializes `filp->private_data` to point to the associated `struct drm_file`,
    // and the object remains valid until the file is released.
    let drm_file_ptr = unsafe { (*filp).private_data.cast::<bindings::drm_file>() };
    if drm_file_ptr.is_null() {
        return bindings::EINVAL as i32;
    }

    // SAFETY: `drm_file_ptr` is a valid pointer to the DRM file associated with
    // this open file descriptor, obtained from `filp->private_data`.
    let drm_file = unsafe { drm::file::File::<TyrDrmFileData>::from_raw(drm_file_ptr) };

    let device = drm_file.device();

    // SAFETY: This is called from the DRM mmap file operation, so `vma` is
    // undergoing initial setup for the duration of the callback.
    let vma_new = unsafe { VmaNew::from_raw(vma) };

    match mmap_user_mmio(device, vma_new) {
        Some(Ok(())) => 0,
        Some(Err(e)) => e.to_errno(),
        // Fall back to the standard DRM GEM mmap path for non-userspace-MMIO
        // mappings. This matches the default GEM fops created by
        // `drm::gem::create_fops()`, whose `.mmap` callback is
        // `bindings::drm_gem_mmap`.
        //
        // SAFETY: Called from mmap context with valid `filp` and `vma`.
        None => unsafe { bindings::drm_gem_mmap(filp, vma) },
    }
}

/// Handles Panthor userspace-MMIO mmap requests.
///
/// If the VMA offset does not refer to the Panthor flush-ID mmap offset, returns
/// [`None`] so the caller can fall back to the normal DRM GEM mmap path.
///
/// If the offset refers to a userspace-MMIO mapping, validates the requested
/// mapping parameters and configures the VMA for MMIO access. Returns
/// `Some(Err(...))` for invalid MMIO mappings and `Some(Ok(()))` on success.
fn mmap_user_mmio(device: &TyrDrmDevice, vma: &VmaNew) -> Option<Result> {
    let pgoff = vma.pgoff() as u64;
    if pgoff < DRM_PANTHOR_USER_MMIO_PGOFF_64BIT {
        return None;
    }

    if pgoff != DRM_PANTHOR_USER_FLUSH_ID_MMIO_PGOFF {
        return Some(Err(EINVAL));
    }

    if (vma.flags() & vma_flags::SHARED) == 0 {
        return Some(Err(EINVAL));
    }

    if vma.end() - vma.start() != PAGE_SIZE {
        return Some(Err(EINVAL));
    }

    if (vma.flags() & (vma_flags::WRITE | vma_flags::EXEC)) != 0 {
        return Some(Err(EINVAL));
    }

    if vma.try_clear_maywrite().is_err() {
        return Some(Err(EINVAL));
    }

    vma.set_io();
    vma.set_dontcopy();
    vma.set_dontexpand();
    vma.set_dontdump();

    let vma_ptr = vma.as_ptr();

    // SAFETY: The VmaNew invariant guarantees the VMA pointer is valid and that the
    // mapping is undergoing initial setup, so mutating VMA fields is safe.
    // vm_private_data stores the DRM device pointer for the fault handler; the DRM
    // device outlives the file and associated VMA.
    unsafe {
        // This VMA maps raw MMIO PFNs rather than normal memory pages.
        (*vma_ptr).__bindgen_anon_2.vm_flags |= vma_flags::PFNMAP | vma_flags::NORESERVE;

        // Store the device pointer so the fault handler can recover it later.
        (*vma_ptr).vm_private_data = core::ptr::from_ref(device).cast_mut().cast();

        // Install the VMA operations for handling MMIO page faults.
        (*vma_ptr).vm_ops = core::ptr::from_ref(&VM_OPS);
    }

    Some(Ok(()))
}

static VM_OPS: bindings::vm_operations_struct = bindings::vm_operations_struct {
    fault: Some(vm_fault_handler),
    // SAFETY: Zero-initializing vm_operations_struct is valid because unset
    // callback fields are represented as NULL function pointers.
    ..unsafe { core::mem::zeroed() }
};

/// Page fault handler for userspace-MMIO VMAs.
///
/// Used for lazy insertion of the MMIO page into the VMA on first access.
///
/// Handles first access to the userspace-MMIO mapping by inserting the
/// corresponding physical MMIO page into the VMA with non-cached protections
/// via [`bindings::vmf_insert_pfn_prot`].
///
/// # Safety
///
/// Must only be called by the kernel fault handling machinery with a valid
/// `vmf` pointer.
unsafe extern "C" fn vm_fault_handler(vmf: *mut bindings::vm_fault) -> bindings::vm_fault_t {
    const VM_FAULT_SIGBUS: bindings::vm_fault_t = bindings::vm_fault_reason_VM_FAULT_SIGBUS;

    // SAFETY: `vmf` is a valid fault descriptor provided by the kernel for this
    // vm_ops.fault callback. Its embedded `vma` pointer and fault address are
    // initialized for the duration of the fault handling operation.
    let (vma, address) = unsafe { ((*vmf).__bindgen_anon_1.vma, (*vmf).__bindgen_anon_1.address) };

    // SAFETY: `vma` comes from a valid fault descriptor, and `vm_private_data`
    // was initialized by mmap_user_mmio() to store the associated Tyr DRM device
    // pointer for this VMA.
    let ddev_ptr = unsafe { (*vma).vm_private_data as *const TyrDrmDevice };
    if ddev_ptr.is_null() {
        return VM_FAULT_SIGBUS;
    }

    // SAFETY: ddev_ptr is non-null, properly aligned, and was originally a shared reference,
    // so the pointee is valid for the lifetime of the device.
    let ddev = unsafe { &*ddev_ptr };

    // SAFETY: vma is valid (obtained from a valid vmf).
    let pgoff = unsafe { (*vma).vm_pgoff } as u64;

    if pgoff != DRM_PANTHOR_USER_FLUSH_ID_MMIO_PGOFF {
        return VM_FAULT_SIGBUS;
    }

    // `vm_start` and `vm_end` are behind bindgen-generated anonymous fields.
    // SAFETY: vma is valid (obtained from a valid vmf).
    let vm_start = unsafe { (*vma).__bindgen_anon_1.__bindgen_anon_1.vm_start };
    // SAFETY: vma is valid (obtained from a valid vmf).
    let vm_end = unsafe { (*vma).__bindgen_anon_1.__bindgen_anon_1.vm_end };

    if address < vm_start || address >= vm_end {
        return VM_FAULT_SIGBUS;
    }

    let phys_addr = ddev.mmio_phys_addr + LATEST_FLUSH::OFFSET as u64;
    let pfn = (phys_addr >> PAGE_SHIFT) as usize;

    // SAFETY: vma is valid (obtained from a valid vmf).
    let pgprot = unsafe { bindings::pgprot_noncached((*vma).vm_page_prot) };

    // SAFETY: vma, address, pfn, and pgprot are all valid.
    let ret = unsafe { bindings::vmf_insert_pfn_prot(vma, address, pfn, pgprot) };

    ret as bindings::vm_fault_t
}
