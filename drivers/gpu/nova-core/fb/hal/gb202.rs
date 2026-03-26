// SPDX-License-Identifier: GPL-2.0

//! Blackwell GB20x framebuffer HAL.
//!
//! GB20x GPUs moved the sysmem flush registers from `NV_PFB_NISO_FLUSH_SYSMEM_ADDR` to
//! `NV_PFB_FBHUB0_PCIE_FLUSH_SYSMEM_ADDR_{LO,HI}`.

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    fb::hal::FbHal,
    regs, //
};

struct Gb202;

fn read_sysmem_flush_page_gb202(bar: &Bar0) -> u64 {
    let lo = u64::from(regs::NV_PFB_FBHUB0_PCIE_FLUSH_SYSMEM_ADDR_LO::read(bar).adr());
    let hi = u64::from(regs::NV_PFB_FBHUB0_PCIE_FLUSH_SYSMEM_ADDR_HI::read(bar).adr());

    lo | (hi << 32)
}

fn write_sysmem_flush_page_gb202(bar: &Bar0, addr: u64) {
    // Write HI first. The hardware will trigger the flush on the LO write.
    regs::NV_PFB_FBHUB0_PCIE_FLUSH_SYSMEM_ADDR_HI::default()
        // CAST: upper 32 bits, then masked to 20 bits by the register field.
        .set_adr((addr >> 32) as u32)
        .write(bar);
    regs::NV_PFB_FBHUB0_PCIE_FLUSH_SYSMEM_ADDR_LO::default()
        // CAST: lower 32 bits. Hardware ignores bits 7:0.
        .set_adr(addr as u32)
        .write(bar);
}

impl FbHal for Gb202 {
    fn read_sysmem_flush_page(&self, bar: &Bar0) -> u64 {
        read_sysmem_flush_page_gb202(bar)
    }

    fn write_sysmem_flush_page(&self, bar: &Bar0, addr: u64) -> Result {
        write_sysmem_flush_page_gb202(bar, addr);

        Ok(())
    }

    fn supports_display(&self, bar: &Bar0) -> bool {
        super::ga100::display_enabled_ga100(bar)
    }

    fn vidmem_size(&self, bar: &Bar0) -> u64 {
        super::ga102::vidmem_size_ga102(bar)
    }

    fn non_wpr_heap_size(&self) -> Option<u32> {
        Some(super::BLACKWELL_NON_WPR_HEAP_SIZE)
    }
}

const GB202: Gb202 = Gb202;
pub(super) const GB202_HAL: &dyn FbHal = &GB202;
