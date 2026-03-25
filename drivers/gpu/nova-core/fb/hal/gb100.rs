// SPDX-License-Identifier: GPL-2.0

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    fb::hal::FbHal, //
};

struct Gb100;

impl FbHal for Gb100 {
    fn read_sysmem_flush_page(&self, bar: &Bar0) -> u64 {
        super::ga100::read_sysmem_flush_page_ga100(bar)
    }

    fn write_sysmem_flush_page(&self, bar: &Bar0, addr: u64) -> Result {
        super::ga100::write_sysmem_flush_page_ga100(bar, addr);

        Ok(())
    }

    fn supports_display(&self, bar: &Bar0) -> bool {
        super::ga100::display_enabled_ga100(bar)
    }

    fn vidmem_size(&self, bar: &Bar0) -> u64 {
        super::ga102::vidmem_size_ga102(bar)
    }

    fn non_wpr_heap_size(&self) -> Option<u32> {
        // 2 MiB + 128 KiB non-WPR heap for Blackwell (see Open RM: kgspCalculateFbLayout_GB100).
        Some(0x220000)
    }
}

const GB100: Gb100 = Gb100;
pub(super) const GB100_HAL: &dyn FbHal = &GB100;
