// SPDX-License-Identifier: GPL-2.0

//! FSP is a hardware unit that runs FMC firmware.

use kernel::{
    device,
    firmware::Firmware,
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
    /// Full FMC ELF for signature extraction.
    pub(crate) fmc_elf: Firmware,
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
        let fmc_image = DmaObject::from_data(dev, fmc_image_data)?;

        Ok(Self {
            fmc_image,
            fmc_elf: fw,
        })
    }
}
