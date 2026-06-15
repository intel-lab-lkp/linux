// SPDX-License-Identifier: GPL-2.0

use core::ops::Range;

use kernel::{
    dma::DmaMask,
    prelude::*, //
};

use crate::{
    driver::Bar0,
    fsp::Fsp,
    gpu::Chipset, //
};

use super::GpuHal;

struct Gh100;

impl GpuHal for Gh100 {
    fn wait_preboot_completion(&self, bar: Bar0<'_>, chipset: Chipset) -> Result {
        Fsp::wait_for_secure_boot(bar, chipset)
    }

    fn dma_mask(&self) -> DmaMask {
        DmaMask::new::<52>()
    }

    fn pci_config_mirror_range(&self) -> Range<u32> {
        const PCI_CONFIG_MIRROR_START: u32 = 0x092000;
        const PCI_CONFIG_MIRROR_SIZE: u32 = 0x001000;

        PCI_CONFIG_MIRROR_START..PCI_CONFIG_MIRROR_START + PCI_CONFIG_MIRROR_SIZE
    }
}

const GH100: Gh100 = Gh100;
pub(super) const GH100_HAL: &dyn GpuHal = &GH100;
