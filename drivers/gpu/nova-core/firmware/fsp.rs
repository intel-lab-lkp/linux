// SPDX-License-Identifier: GPL-2.0

//! FSP is a hardware unit that runs FMC firmware.

use kernel::{
    alloc::KVec,
    device,
    prelude::*, //
};

use crate::{
    dma::DmaObject,
    firmware::elf,
    gpu::Chipset, //
};

pub(crate) struct FspFirmware {
    /// FMC firmware image data (only the "image" ELF section).
    pub(crate) fmc_image: DmaObject,
    /// Full FMC ELF data (for signature extraction).
    pub(crate) fmc_full: KVec<u8>,
}

impl FspFirmware {
    pub(crate) fn new(
        dev: &device::Device<device::Bound>,
        chipset: Chipset,
        ver: &str,
    ) -> Result<Self> {
        let fw = super::request_firmware(dev, chipset, "fmc", ver)?;

        // FSP expects only the "image" section, not the entire ELF file.
        let fmc_image_data = elf::elf_section(fw.data(), "image").ok_or_else(|| {
            dev_err!(dev, "FMC ELF file missing 'image' section\n");
            EINVAL
        })?;

        // Copy the full ELF into a kernel vector for CPU-side signature extraction.
        let mut fmc_full = KVec::with_capacity(fw.data().len(), GFP_KERNEL)?;
        fmc_full.extend_from_slice(fw.data(), GFP_KERNEL)?;

        Ok(Self {
            fmc_image: DmaObject::from_data(dev, fmc_image_data)?,
            fmc_full,
        })
    }
}
