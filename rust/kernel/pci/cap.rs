// SPDX-License-Identifier: GPL-2.0

//! PCI extended capability support.

use super::{
    ConfigSpace,
    Extended, //
};
use crate::{
    bindings,
    io::{
        Io,
        IoCapable,
        KnownSize,
        Region, //
    },
    prelude::*, //
};

/// PCI extended capability IDs.
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExtCapId {
    /// Single Root I/O Virtualization.
    // CAST: `PCI_EXT_CAP_ID_SRIOV` is `0x10`, which fits in `u16`.
    Sriov = bindings::PCI_EXT_CAP_ID_SRIOV as u16,
}

impl ExtCapId {
    fn as_raw(self) -> u16 {
        self as u16
    }
}

/// An extended PCI capability that implements [`Io`].
///
/// # Examples
///
/// ```no_run
/// use kernel::pci::{
///     self,
///     ExtSriovCapability, //
/// };
/// use kernel::io::Io;
///
/// fn probe_sriov(pdev: &pci::Device<kernel::device::Core>) -> Result<(), kernel::error::Error> {
///     let config = pdev.config_space_extended()?;
///     let sriov = ExtSriovCapability::find(&config)?;
///
///     let total_vfs = kernel::io_read!(&sriov, .total_vfs);
///     let vf_offset = kernel::io_read!(&sriov, .vf_offset);
///     let bar0 = kernel::io_read!(&sriov, .vf_bar[0]);
///     kernel::io_write!(&sriov, .num_vfs, 4u16);
///     let bar0_64 = sriov.read_vf_bar64(0)?;
///
///     Ok(())
/// }
/// ```
///
/// # Invariants
///
/// `ptr` is within the device's extended configuration space at a valid
/// capability. For sized `T`, the region is at least `size_of::<T>()` bytes.
pub struct ExtCapability<'a, T: ?Sized + KnownSize = Region<0>> {
    config: &'a ConfigSpace<'a, Extended>,
    ptr: *mut T,
}

impl<T: ?Sized + KnownSize> Io for ExtCapability<'_, T> {
    type Type = T;

    #[inline]
    fn as_ptr(&self) -> *mut T {
        self.ptr
    }
}

macro_rules! impl_ext_cap_io_capable {
    ($ty:ty) => {
        impl<T: ?Sized + KnownSize> IoCapable<$ty> for ExtCapability<'_, T> {
            #[inline]
            unsafe fn io_read(&self, address: *mut $ty) -> $ty {
                // SAFETY: The caller guarantees `address` is within bounds of
                // this capability, which is within the config space.
                unsafe { self.config.io_read(address) }
            }

            #[inline]
            unsafe fn io_write(&self, value: $ty, address: *mut $ty) {
                // SAFETY: The caller guarantees `address` is within bounds of
                // this capability, which is within the config space.
                unsafe { self.config.io_write(value, address) }
            }
        }
    };
}

impl_ext_cap_io_capable!(u8);
impl_ext_cap_io_capable!(u16);
impl_ext_cap_io_capable!(u32);

impl<'a> ExtCapability<'a> {
    /// Base offset of this capability in configuration space.
    #[inline]
    pub fn offset(&self) -> usize {
        self.ptr.addr()
    }

    /// Size of this capability region in bytes.
    #[inline]
    pub fn size(&self) -> usize {
        KnownSize::size(self.ptr)
    }

    /// Cast to a typed capability, checking that the region is large enough.
    pub fn cast_sized<U>(self) -> Result<ExtCapability<'a, U>> {
        if self.size() < core::mem::size_of::<U>() {
            return Err(EINVAL);
        }

        Ok(ExtCapability {
            config: self.config,
            ptr: core::ptr::without_provenance_mut(self.offset()),
        })
    }
}

impl ConfigSpace<'_, Extended> {
    /// Finds an extended capability by ID, returning an untyped [`ExtCapability`].
    pub fn find_ext_capability(&self, cap: ExtCapId) -> Result<ExtCapability<'_>> {
        let offset = usize::from(
            // SAFETY: `self.pdev` is valid by the type invariant of `ConfigSpace`.
            unsafe { bindings::pci_find_ext_capability(self.pdev.as_raw(), i32::from(cap.as_raw())) },
        );

        if offset == 0 {
            return Err(ENODEV);
        }

        Ok(self.make_ext_capability(offset))
    }

    /// Finds the next extended capability with `cap` after `start`.
    pub fn find_next_ext_capability(&self, start: u16, cap: ExtCapId) -> Result<ExtCapability<'_>> {
        let offset = usize::from(
            // SAFETY: `self.pdev` is valid by the type invariant of `ConfigSpace`.
            unsafe {
                bindings::pci_find_next_ext_capability(self.pdev.as_raw(), start, i32::from(cap.as_raw()))
            },
        );

        if offset == 0 {
            return Err(ENODEV);
        }

        Ok(self.make_ext_capability(offset))
    }

    fn make_ext_capability(&self, offset: usize) -> ExtCapability<'_> {
        let size = self.calculate_ext_cap_size(offset);

        let ptr = core::ptr::slice_from_raw_parts_mut::<u8>(
            core::ptr::without_provenance_mut(offset),
            size,
        // CAST: `Region<0>` is a DST like `[u8]`, so this pointer cast preserves metadata.
        ) as *mut Region<0>;

        ExtCapability {
            config: self,
            ptr,
        }
    }

    fn calculate_ext_cap_size(&self, offset: usize) -> usize {
        let header = self.try_read32(offset).unwrap_or(0);
        // SAFETY: Pure bit manipulation, no preconditions.
        // CAST: The next-cap pointer is a 12-bit field (max 0xFFC), always fits in `usize`.
        let next_ptr = unsafe { bindings::pci_ext_cap_next(header) } as usize;

        if next_ptr == 0 {
            KnownSize::size(self.as_ptr()) - offset
        } else {
            next_ptr - offset
        }
    }
}

/// SR-IOV register layout per PCIe spec (64 bytes starting at cap offset).
#[repr(C)]
pub struct ExtSriovRegs {
    /// Extended capability header.
    pub header: u32,
    /// SR-IOV capabilities.
    pub cap: u32,
    /// SR-IOV control.
    pub ctrl: u16,
    /// SR-IOV status.
    pub status: u16,
    /// Initial VFs.
    pub initial_vfs: u16,
    /// Total VFs.
    pub total_vfs: u16,
    /// Number of VFs.
    pub num_vfs: u16,
    /// Function dependency link.
    pub func_dep_link: u16,
    /// First VF offset.
    pub vf_offset: u16,
    /// VF stride.
    pub vf_stride: u16,
    _reserved: u16,
    /// VF device ID.
    pub vf_device_id: u16,
    /// Supported page sizes.
    pub supported_page_sizes: u32,
    /// System page size.
    pub system_page_size: u32,
    /// VF BARs (BAR0–BAR5).
    pub vf_bar: [u32; 6],
    /// VF migration state array offset.
    pub migration_state: u32,
}

/// SR-IOV capability. See [`ExtCapability`] for usage.
pub type ExtSriovCapability<'a> = ExtCapability<'a, ExtSriovRegs>;

impl ExtCapability<'_, ExtSriovRegs> {
    /// Find the SR-IOV capability, or `ENODEV` if not present.
    pub fn find<'a>(config: &'a ConfigSpace<'_, Extended>) -> Result<ExtCapability<'a, ExtSriovRegs>> {
        config
            .find_ext_capability(ExtCapId::Sriov)?
            .cast_sized()
    }

    /// Reads a 64-bit VF BAR from two consecutive 32-bit slots.
    pub fn read_vf_bar64(&self, bar_index: usize) -> Result<u64> {
        if bar_index >= 5 {
            return Err(EINVAL);
        }
        let low = crate::io_read!(self, .vf_bar[bar_index]?);
        let high = crate::io_read!(self, .vf_bar[bar_index + 1]?);
        Ok((u64::from(high) << 32) | u64::from(low))
    }
}
