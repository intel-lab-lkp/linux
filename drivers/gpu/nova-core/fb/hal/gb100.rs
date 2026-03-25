// SPDX-License-Identifier: GPL-2.0

//! Blackwell GB10x framebuffer HAL.
//!
//! GB10x GPUs use HSHUB0 registers for the sysmem flush page. Both the primary and EG (egress)
//! register pairs must be programmed to the same address, as required by hardware.

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    fb::hal::FbHal,
    regs, //
};

struct Gb100;

fn read_sysmem_flush_page_gb100(bar: &Bar0) -> u64 {
    let lo = u64::from(regs::NV_PFB_HSHUB0_PCIE_FLUSH_SYSMEM_ADDR_LO::read(bar).adr());
    let hi = u64::from(regs::NV_PFB_HSHUB0_PCIE_FLUSH_SYSMEM_ADDR_HI::read(bar).adr());

    lo | (hi << 32)
}

fn write_sysmem_flush_page_gb100(bar: &Bar0, addr: u64) {
    // CAST: lower 32 bits. Hardware ignores bits 7:0.
    let addr_lo = addr as u32;
    // CAST: upper 32 bits, then masked to 20 bits by the register field.
    let addr_hi = (addr >> 32) as u32;

    // Write HI first. The hardware will trigger the flush on the LO write.

    // Primary HSHUB pair.
    regs::NV_PFB_HSHUB0_PCIE_FLUSH_SYSMEM_ADDR_HI::default()
        .set_adr(addr_hi)
        .write(bar);
    regs::NV_PFB_HSHUB0_PCIE_FLUSH_SYSMEM_ADDR_LO::default()
        .set_adr(addr_lo)
        .write(bar);

    // EG (egress) pair -- must match the primary pair.
    regs::NV_PFB_HSHUB0_EG_PCIE_FLUSH_SYSMEM_ADDR_HI::default()
        .set_adr(addr_hi)
        .write(bar);
    regs::NV_PFB_HSHUB0_EG_PCIE_FLUSH_SYSMEM_ADDR_LO::default()
        .set_adr(addr_lo)
        .write(bar);
}

impl FbHal for Gb100 {
    fn read_sysmem_flush_page(&self, bar: &Bar0) -> u64 {
        read_sysmem_flush_page_gb100(bar)
    }

    fn write_sysmem_flush_page(&self, bar: &Bar0, addr: u64) -> Result {
        write_sysmem_flush_page_gb100(bar, addr);

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

const GB100: Gb100 = Gb100;
pub(super) const GB100_HAL: &dyn FbHal = &GB100;
