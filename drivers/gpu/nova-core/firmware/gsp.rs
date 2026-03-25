// SPDX-License-Identifier: GPL-2.0

use core::mem::size_of_val;

use kernel::{
    device,
    dma::{
        DataDirection,
        DmaAddress, //
    },
    kvec,
    prelude::*,
    scatterlist::{
        Owned,
        SGTable, //
    },
};

use crate::{
    dma::DmaObject,
    firmware::{
        elf,
        riscv::RiscvFirmware, //
    },
    gpu::{
        Architecture,
        Chipset, //
    },
    gsp::GSP_PAGE_SIZE,
    num::FromSafeCast,
};

/// GSP firmware with 3-level radix page tables for the GSP bootloader.
///
/// The bootloader expects firmware to be mapped starting at address 0 in GSP's virtual address
/// space:
///
/// ```text
/// Level 0:  1 page, 1 entry         -> points to first level 1 page
/// Level 1:  Multiple pages/entries  -> each entry points to a level 2 page
/// Level 2:  Multiple pages/entries  -> each entry points to a firmware page
/// ```
///
/// Each page is 4KB, each entry is 8 bytes (64-bit DMA address).
/// Also known as "Radix3" firmware.
#[pin_data]
pub(crate) struct GspFirmware {
    /// The GSP firmware inside a [`VVec`], device-mapped via a SG table.
    #[pin]
    fw: SGTable<Owned<VVec<u8>>>,
    /// Level 2 page table whose entries contain DMA addresses of firmware pages.
    #[pin]
    level2: SGTable<Owned<VVec<u8>>>,
    /// Level 1 page table whose entries contain DMA addresses of level 2 pages.
    #[pin]
    level1: SGTable<Owned<VVec<u8>>>,
    /// Level 0 page table (single 4KB page) with one entry: DMA address of first level 1 page.
    level0: DmaObject,
    /// Size in bytes of the firmware contained in [`Self::fw`].
    pub(crate) size: usize,
    /// Device-mapped GSP signatures matching the GPU's [`Chipset`].
    pub(crate) signatures: DmaObject,
    /// GSP bootloader, verifies the GSP firmware before loading and running it.
    pub(crate) bootloader: RiscvFirmware,
}

impl GspFirmware {
    fn find_gsp_sigs_section(chipset: Chipset) -> Option<&'static str> {
        match chipset.arch() {
            Architecture::Turing if matches!(chipset, Chipset::TU116 | Chipset::TU117) => {
                Some(".fwsignature_tu11x")
            }
            Architecture::Turing => Some(".fwsignature_tu10x"),
            // GA100 uses the same firmware as Turing
            Architecture::Ampere if chipset == Chipset::GA100 => Some(".fwsignature_tu10x"),
            Architecture::Ampere => Some(".fwsignature_ga10x"),
            Architecture::Ada => Some(".fwsignature_ad10x"),
            Architecture::Hopper => Some(".fwsignature_gh10x"),
            Architecture::Blackwell if matches!(chipset, Chipset::GB100 | Chipset::GB102) => {
                Some(".fwsignature_gb10x")
            }
            Architecture::Blackwell => Some(".fwsignature_gb20x"),
        }
    }

    /// Loads the GSP firmware binaries, map them into `dev`'s address-space, and creates the page
    /// tables expected by the GSP bootloader to load it.
    pub(crate) fn new<'a>(
        dev: &'a device::Device<device::Bound>,
        chipset: Chipset,
        ver: &'a str,
    ) -> impl PinInit<Self, Error> + 'a {
        pin_init::pin_init_scope(move || {
            let firmware = super::request_firmware(dev, chipset, "gsp", ver)?;

            let fw_section = elf::elf_section(firmware.data(), ".fwimage").ok_or(EINVAL)?;

            let size = fw_section.len();

            // Move the firmware into a vmalloc'd vector and map it into the device address
            // space.
            let fw_vvec = VVec::with_capacity(fw_section.len(), GFP_KERNEL)
                .and_then(|mut v| {
                    v.extend_from_slice(fw_section, GFP_KERNEL)?;
                    Ok(v)
                })
                .map_err(|_| ENOMEM)?;

            Ok(try_pin_init!(Self {
                fw <- SGTable::new(dev, fw_vvec, DataDirection::ToDevice, GFP_KERNEL),
                level2 <- {
                    // Allocate the level 2 page table, map the firmware onto it, and map it into
                    // the device address space.
                    VVec::<u8>::with_capacity(
                        fw.iter().count() * core::mem::size_of::<u64>(),
                        GFP_KERNEL,
                    )
                    .map_err(|_| ENOMEM)
                    .and_then(|level2| map_into_lvl(&fw, level2))
                    .map(|level2| SGTable::new(dev, level2, DataDirection::ToDevice, GFP_KERNEL))?
                },
                level1 <- {
                    // Allocate the level 1 page table, map the level 2 page table onto it, and map
                    // it into the device address space.
                    VVec::<u8>::with_capacity(
                        level2.iter().count() * core::mem::size_of::<u64>(),
                        GFP_KERNEL,
                    )
                    .map_err(|_| ENOMEM)
                    .and_then(|level1| map_into_lvl(&level2, level1))
                    .map(|level1| SGTable::new(dev, level1, DataDirection::ToDevice, GFP_KERNEL))?
                },
                level0: {
                    // Allocate the level 0 page table as a device-visible DMA object, and map the
                    // level 1 page table onto it.

                    // Level 0 page table data.
                    let mut level0_data = kvec![0u8; GSP_PAGE_SIZE]?;

                    // Fill level 1 page entry.
                    let level1_entry = level1.iter().next().ok_or(EINVAL)?;
                    let level1_entry_addr = level1_entry.dma_address();
                    let dst = &mut level0_data[..size_of_val(&level1_entry_addr)];
                    dst.copy_from_slice(&level1_entry_addr.to_le_bytes());

                    // Turn the level0 page table into a [`DmaObject`].
                    DmaObject::from_data(dev, &level0_data)?
                },
                size,
                signatures: {
                    let sigs_section = Self::find_gsp_sigs_section(chipset).ok_or(ENOTSUPP)?;

                    elf::elf_section(firmware.data(), sigs_section)
                        .ok_or(EINVAL)
                        .and_then(|data| DmaObject::from_data(dev, data))?
                },
                bootloader: {
                    let bl = super::request_firmware(dev, chipset, "bootloader", ver)?;

                    RiscvFirmware::new(dev, &bl)?
                },
            }))
        })
    }

    /// Returns the DMA handle of the radix3 level 0 page table.
    pub(crate) fn radix3_dma_handle(&self) -> DmaAddress {
        self.level0.dma_handle()
    }
}

/// Build a page table from a scatter-gather list.
///
/// Takes each DMA-mapped region from `sg_table` and writes page table entries
/// for all 4KB pages within that region. For example, a 16KB SG entry becomes
/// 4 consecutive page table entries.
fn map_into_lvl(sg_table: &SGTable<Owned<VVec<u8>>>, mut dst: VVec<u8>) -> Result<VVec<u8>> {
    for sg_entry in sg_table.iter() {
        // Number of pages we need to map.
        let num_pages = usize::from_safe_cast(sg_entry.dma_len()).div_ceil(GSP_PAGE_SIZE);

        for i in 0..num_pages {
            let entry = sg_entry.dma_address()
                + (u64::from_safe_cast(i) * u64::from_safe_cast(GSP_PAGE_SIZE));
            dst.extend_from_slice(&entry.to_le_bytes(), GFP_KERNEL)?;
        }
    }

    Ok(dst)
}
