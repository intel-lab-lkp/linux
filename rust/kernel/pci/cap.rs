// SPDX-License-Identifier: GPL-2.0

//! PCI extended capability support.

use super::{
    io::ConfigSpaceBackend,
    ConfigSpace,
    Extended, //
};
use crate::{
    bindings,
    io::{
        Io,
        IoBackend,
        Region, //
    },
    prelude::*,
    ptr::KnownSize, //
};

/// Number of VF BAR register slots in an SR-IOV capability.
// CAST: `PCI_SRIOV_NUM_BARS` is 6, which fits in `usize`.
const NUM_VF_BARS: usize = bindings::PCI_SRIOV_NUM_BARS as usize;

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

/// A typed PCI extended capability register layout.
///
/// Implementors describe the register layout of one extended capability. The layout must start at
/// the extended capability header, and [`Self::ID`] must identify that layout.
pub trait ExtCapability: FromBytes + IntoBytes {
    /// PCI extended capability ID for this register layout.
    const ID: ExtCapId;
}

impl ConfigSpace<'_, Region<0>> {
    /// Base offset of this capability in configuration space.
    #[inline]
    pub fn offset(&self) -> usize {
        ConfigSpaceBackend::as_ptr(*self).addr()
    }

    /// Size of this capability region in bytes.
    #[inline]
    pub fn size(&self) -> usize {
        KnownSize::size(ConfigSpaceBackend::as_ptr(*self))
    }
}

impl<'a> ConfigSpace<'a, Extended> {
    /// Finds and projects an extended capability into its typed register layout.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// use kernel::pci;
    ///
    /// fn probe_sriov(
    ///     pdev: &pci::Device<kernel::device::Bound>,
    /// ) -> Result<(), kernel::error::Error> {
    ///     let sriov = pdev
    ///         .config_space_extended()?
    ///         .find_ext_capability::<pci::ExtSriovRegs>()?;
    ///
    ///     let total_vfs = kernel::io_read!(sriov, .total_vfs);
    ///     let vf_offset = kernel::io_read!(sriov, .vf_offset);
    ///     let bar0 = kernel::io_read!(sriov, .vf_bar[build: 0]);
    ///     kernel::io_write!(sriov, .num_vfs, 4u16);
    ///     let bar0_64 = sriov.read_vf_bar64(0)?;
    ///
    ///     Ok(())
    /// }
    /// ```
    pub fn find_ext_capability<C: ExtCapability>(&self) -> Result<ConfigSpace<'a, C>> {
        let offset = usize::from(
            // SAFETY: `self.pdev` is valid by the type invariant of `ConfigSpace`.
            unsafe {
                bindings::pci_find_ext_capability(self.pdev.as_raw(), i32::from(C::ID.as_raw()))
            },
        );

        if offset == 0 {
            return Err(ENODEV);
        }

        let size = self.calculate_ext_cap_size(offset);

        let base = ConfigSpaceBackend::as_ptr(*self)
            .cast::<u8>()
            .wrapping_add(offset);
        let ptr = Region::<0>::ptr_try_from_raw_parts_mut(base, size)?;

        // SAFETY: `offset` was returned by `pci_find_ext_capability`, and
        // `calculate_ext_cap_size` bounds `ptr` at the next capability or the end of the extended
        // configuration space. `ptr_try_from_raw_parts_mut` verified the region layout.
        let capability = unsafe { ConfigSpaceBackend::project_view(*self, ptr) };

        capability.try_cast::<C>()
    }

    /// Calculates the size of the extended capability at `offset`.
    ///
    /// The capability extends to the next extended capability, or to the end of the extended
    /// configuration space if it is the last one. `offset` must be a DWORD-aligned offset within
    /// the extended configuration space returned by `pci_find_ext_capability`. If its header
    /// cannot be read, the capability is treated as the last one.
    fn calculate_ext_cap_size(&self, offset: usize) -> usize {
        let header = self.try_read32(offset).unwrap_or(0);
        // SAFETY: Pure bit manipulation, no preconditions.
        // CAST: The next-cap pointer is a 12-bit field (max 0xFFC), always fits in `usize`.
        let next = unsafe { bindings::pci_ext_cap_next(header) } as usize;

        if next > offset {
            next - offset
        } else {
            (*self).size() - offset
        }
    }
}

/// SR-IOV register layout per PCIe spec (64 bytes starting at cap offset).
#[repr(C)]
#[derive(FromBytes, IntoBytes)]
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
    pub vf_bar: [u32; NUM_VF_BARS],
    /// VF migration state array offset.
    pub migration_state: u32,
}

impl ExtCapability for ExtSriovRegs {
    const ID: ExtCapId = ExtCapId::Sriov;
}

/// A typed view of an SR-IOV extended capability.
pub type ExtSriovCapability<'a> = ConfigSpace<'a, ExtSriovRegs>;

impl ConfigSpace<'_, ExtSriovRegs> {
    /// Returns `true` if the VF BAR at `bar_index` is a 64-bit memory BAR.
    #[inline]
    pub fn is_vf_bar_64bit(&self, bar_index: usize) -> Result<bool> {
        if bar_index >= NUM_VF_BARS {
            return Err(EINVAL);
        }

        let bar = crate::io_read!(*self, .vf_bar[try: bar_index]);

        Ok(bar & bindings::PCI_BASE_ADDRESS_SPACE == 0
            && bar & bindings::PCI_BASE_ADDRESS_MEM_TYPE_MASK
                == bindings::PCI_BASE_ADDRESS_MEM_TYPE_64)
    }

    /// Reads a 64-bit VF BAR from two consecutive 32-bit slots.
    #[inline]
    pub fn read_vf_bar64(&self, bar_index: usize) -> Result<u64> {
        if !self.is_vf_bar_64bit(bar_index)? {
            return Err(EINVAL);
        }

        let high_index = bar_index.checked_add(1).ok_or(EINVAL)?;
        if high_index >= NUM_VF_BARS {
            return Err(EINVAL);
        }

        let low = crate::io_read!(*self, .vf_bar[try: bar_index]);
        let high = crate::io_read!(*self, .vf_bar[try: high_index]);
        Ok((u64::from(high) << 32) | u64::from(low))
    }
}
