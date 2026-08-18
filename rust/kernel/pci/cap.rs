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
    num::Bounded,
    prelude::*,
};

/// Number of VF BAR register slots in an SR-IOV capability.
// CAST: `PCI_SRIOV_NUM_BARS` is the PCIe-specified number of VF BAR register slots and fits in
// `usize`.
const NUM_VF_BARS: usize = bindings::PCI_SRIOV_NUM_BARS as usize;

/// PCI extended capability IDs.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExtCapId(u16);

impl ExtCapId {
    /// Single Root I/O Virtualization.
    // CAST: PCI extended capability IDs are 16-bit values defined by the PCIe specification.
    pub const SRIOV: Self = Self(bindings::PCI_EXT_CAP_ID_SRIOV as u16);

    /// Creates an extended capability ID from its raw PCIe value.
    #[inline]
    pub const fn new(id: u16) -> Self {
        Self(id)
    }

    /// Returns the raw PCIe extended capability ID.
    #[inline]
    const fn as_raw(self) -> u16 {
        self.0
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

impl<'a> ConfigSpace<'a, Extended> {
    /// Finds and projects an extended capability into its typed register layout.
    ///
    /// Returns [`None`] if the device does not implement the capability.
    ///
    /// Returns an error if the capability is present but its register span is too small or
    /// insufficiently aligned for `C`.
    ///
    /// # Examples
    ///
    /// ```no_run
    /// use kernel::{
    ///     device::Bound,
    ///     io::io_read,
    ///     pci,
    ///     prelude::*,
    /// };
    ///
    /// fn probe_sriov(pdev: &pci::Device<Bound>) -> Result {
    ///     let Some(sriov) = pdev
    ///         .config_space_extended()?
    ///         .find_ext_capability::<pci::ExtSriovRegs>()?
    ///     else {
    ///         return Ok(());
    ///     };
    ///
    ///     let total_vfs = io_read!(sriov, .total_vfs);
    ///     let vf_offset = io_read!(sriov, .vf_offset);
    ///     let mut vf_bars = sriov.vf_bars()?;
    ///     let bar0 = vf_bars.next().ok_or(EINVAL)?;
    ///     let bar1 = vf_bars.next().ok_or(EINVAL)?;
    ///     let bar2 = vf_bars.next().ok_or(EINVAL)?;
    ///
    ///     Ok(())
    /// }
    /// ```
    pub fn find_ext_capability<C: ExtCapability>(&self) -> Result<Option<ConfigSpace<'a, C>>> {
        let offset = usize::from(
            // SAFETY: `self.pdev` is valid by the type invariant of `ConfigSpace`.
            unsafe {
                bindings::pci_find_ext_capability(self.pdev.as_raw(), i32::from(C::ID.as_raw()))
            },
        );

        if offset == 0 {
            return Ok(None);
        }

        let size = self.calculate_ext_cap_size(offset)?;

        let base = ConfigSpaceBackend::as_ptr(*self)
            .cast::<u8>()
            .wrapping_add(offset);
        let ptr = Region::<0>::ptr_try_from_raw_parts_mut(base, size)?;

        // SAFETY: `offset` was returned by `pci_find_ext_capability`, and
        // `calculate_ext_cap_size` bounds `ptr` at the next capability or the end of the extended
        // configuration space. `ptr_try_from_raw_parts_mut` verified the region layout.
        let capability = unsafe { ConfigSpaceBackend::project_view(*self, ptr) };

        capability.try_cast::<C>().map(Some)
    }

    /// Calculates the size of the extended capability at `offset`.
    ///
    /// The capability extends to the next extended capability, or to the end of the extended
    /// configuration space if it is the last one. `offset` must be a DWORD-aligned offset within
    /// the extended configuration space returned by `pci_find_ext_capability`. Returns an error if
    /// the capability header is outside the extended configuration space.
    fn calculate_ext_cap_size(&self, offset: usize) -> Result<usize> {
        let header = self.try_read32(offset)?;
        // SAFETY: Pure bit manipulation, no preconditions.
        // CAST: The next-cap pointer is a 12-bit field (max 0xFFC), always fits in `usize`.
        let next = unsafe { bindings::pci_ext_cap_next(header) } as usize;

        Ok(if next > offset {
            next - offset
        } else {
            self.size() - offset
        })
    }
}

/// SR-IOV register layout per PCIe spec (64 bytes starting at cap offset).
#[repr(C)]
#[derive(FromBytes, IntoBytes)]
pub struct ExtSriovRegs {
    /// Extended capability header.
    _header: u32,
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
    pub func_dep_link: u8,
    _reserved_0: u8,
    /// First VF offset.
    pub vf_offset: u16,
    /// VF stride.
    pub vf_stride: u16,
    _reserved_1: u16,
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
    const ID: ExtCapId = ExtCapId::SRIOV;
}

/// A typed view of an SR-IOV extended capability.
pub type ExtSriovCapability<'a> = ConfigSpace<'a, ExtSriovRegs>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum VfBarMemoryType {
    Bits32,
    Bits64,
}

impl TryFrom<Bounded<u32, 2>> for VfBarMemoryType {
    type Error = Error;

    fn try_from(value: Bounded<u32, 2>) -> Result<Self> {
        match value.get() {
            0b00 => Ok(Self::Bits32),
            0b10 => Ok(Self::Bits64),
            _ => Err(EINVAL),
        }
    }
}

impl From<VfBarMemoryType> for Bounded<u32, 2> {
    fn from(value: VfBarMemoryType) -> Self {
        match value {
            VfBarMemoryType::Bits32 => Self::new::<0b00>(),
            VfBarMemoryType::Bits64 => Self::new::<0b10>(),
        }
    }
}

crate::bitfield! {
    /// Low DWORD of an SR-IOV VF BAR.
    struct VfBarLow(u32) {
        /// Base address bits 31:4.
        31:4 address;
        /// Whether the address range is prefetchable.
        3:3 prefetchable => bool;
        /// Memory BAR type.
        2:1 memory_type ?=> VfBarMemoryType;
        /// Whether this is an I/O-space BAR.
        0:0 io_space => bool;
    }
}

/// A decoded VF BAR register encoding.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ExtSriovVfBar {
    /// The BAR address without PCI attribute bits.
    pub address: u64,

    /// Whether the BAR is 64-bit.
    pub is_64bit: bool,
}

/// Iterator over decoded VF BAR register encodings.
///
/// A 32-bit memory BAR encoding uses one register. A 64-bit memory BAR encoding uses that register
/// for bits 31:0 and the immediately following register for bits 63:32.
///
/// # Invariants
///
/// - `next_bar <= bar_count <= NUM_VF_BARS`.
/// - Entries before `bar_count` contain decoded VF BARs in logical order.
struct ExtSriovVfBars {
    bars: [ExtSriovVfBar; NUM_VF_BARS],
    bar_count: usize,
    next_bar: usize,
}

impl ExtSriovVfBars {
    fn new(slots: [u32; NUM_VF_BARS]) -> Result<Self> {
        let mut bars = [ExtSriovVfBar {
            address: 0,
            is_64bit: false,
        }; NUM_VF_BARS];
        let mut bar_count = 0;
        let mut config_slot = 0;

        while config_slot < NUM_VF_BARS {
            let low = VfBarLow::from(slots[config_slot]);

            if low.io_space() {
                return Err(EINVAL);
            }

            let is_64bit = low.memory_type()? == VfBarMemoryType::Bits64;
            let low_address = u64::from(low.address()) << VfBarLow::ADDRESS_SHIFT;

            let address = if is_64bit {
                if config_slot + 1 >= NUM_VF_BARS {
                    return Err(EINVAL);
                }

                let high = slots[config_slot + 1];
                config_slot += 2;
                (u64::from(high) << 32) | low_address
            } else {
                config_slot += 1;
                low_address
            };

            bars[bar_count] = ExtSriovVfBar { address, is_64bit };
            bar_count += 1;
        }

        Ok(Self {
            bars,
            bar_count,
            next_bar: 0,
        })
    }
}

impl Iterator for ExtSriovVfBars {
    type Item = ExtSriovVfBar;

    fn next(&mut self) -> Option<Self::Item> {
        if self.next_bar >= self.bar_count {
            return None;
        }

        let bar = self.bars[self.next_bar];
        self.next_bar += 1;
        Some(bar)
    }
}

impl ExtSriovCapability<'_> {
    /// Returns an iterator over decoded VF BAR register encodings.
    ///
    /// All six raw VF BAR register slots are read and decoded up front. A 32-bit encoding yields
    /// one entry; a 64-bit encoding combines two slots into one entry.
    ///
    /// A zero-valued low DWORD is yielded as a 32-bit BAR at address zero; this method does not
    /// probe whether a BAR is implemented.
    ///
    /// Returns [`EINVAL`] and logs an error if a BAR low DWORD does not encode a 32-bit or 64-bit
    /// memory BAR, or if a 64-bit encoding has no upper DWORD.
    pub fn vf_bars(&self) -> Result<impl Iterator<Item = ExtSriovVfBar>> {
        let slots: [u32; NUM_VF_BARS] =
            core::array::from_fn(|slot| crate::io_read!(*self, .vf_bar[panic: slot]));

        ExtSriovVfBars::new(slots).inspect_err(|_| {
            dev_err!(self.pdev, "invalid VF BAR encoding in SR-IOV capability\n");
        })
    }
}
