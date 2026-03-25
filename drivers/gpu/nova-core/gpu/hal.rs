// SPDX-License-Identifier: GPL-2.0

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    gfw,
    gpu::{
        Architecture,
        Chipset, //
    },
};

pub(crate) trait GpuHal {
    /// Waits for GFW_BOOT completion if required by this hardware family.
    fn wait_gfw_boot_completion(&self, bar: &Bar0) -> Result;
}

struct Tu102;
struct Gh100;

impl GpuHal for Tu102 {
    fn wait_gfw_boot_completion(&self, bar: &Bar0) -> Result {
        gfw::wait_gfw_boot_completion(bar)
    }
}

impl GpuHal for Gh100 {
    fn wait_gfw_boot_completion(&self, _bar: &Bar0) -> Result {
        Ok(())
    }
}

const TU102: Tu102 = Tu102;
const GH100: Gh100 = Gh100;

pub(super) fn gpu_hal(chipset: Chipset) -> &'static dyn GpuHal {
    match chipset.arch() {
        Architecture::Turing | Architecture::Ampere | Architecture::Ada => &TU102,
        Architecture::Hopper | Architecture::Blackwell => &GH100,
    }
}
