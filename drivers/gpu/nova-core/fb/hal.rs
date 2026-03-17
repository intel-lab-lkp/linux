// SPDX-License-Identifier: GPL-2.0

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    gpu::{
        Architecture,
        Chipset, //
    },
};

mod ga100;
mod ga102;
mod gb100;
mod gh100;
mod tu102;

pub(crate) trait FbHal {
    /// Returns the address of the currently-registered sysmem flush page.
    fn read_sysmem_flush_page(&self, bar: &Bar0) -> u64;

    /// Register `addr` as the address of the sysmem flush page.
    ///
    /// This might fail if the address is too large for the receiving register.
    fn write_sysmem_flush_page(&self, bar: &Bar0, addr: u64) -> Result;

    /// Returns `true` is display is supported.
    fn supports_display(&self, bar: &Bar0) -> bool;

    /// Returns the VRAM size, in bytes.
    fn vidmem_size(&self, bar: &Bar0) -> u64;

    /// Returns the non-WPR heap size for GPUs that need large reserved memory.
    ///
    /// Returns `None` for GPUs that don't need extra reserved memory.
    fn non_wpr_heap_size(&self) -> Option<u32> {
        None
    }
}

/// Returns the HAL corresponding to `chipset`.
pub(crate) fn fb_hal(chipset: Chipset) -> &'static dyn FbHal {
    match chipset.arch() {
        Architecture::Turing => tu102::TU102_HAL,
        Architecture::Ampere if chipset == Chipset::GA100 => ga100::GA100_HAL,
        Architecture::Ampere | Architecture::Ada => ga102::GA102_HAL,
        Architecture::Hopper => gh100::GH100_HAL,
        Architecture::Blackwell => gb100::GB100_HAL,
    }
}
