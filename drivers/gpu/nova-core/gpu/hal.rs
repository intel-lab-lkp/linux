// SPDX-License-Identifier: GPL-2.0

use crate::gpu::{
    Architecture,
    Chipset, //
};

pub(crate) trait GpuHal {
    /// Returns whether this hardware family still requires waiting for GFW_BOOT.
    fn needs_gfw_boot(&self) -> bool;
}

struct Tu102;
struct Ga100;
struct Ga102;
struct Fsp;

impl GpuHal for Tu102 {
    fn needs_gfw_boot(&self) -> bool {
        true
    }
}

impl GpuHal for Ga100 {
    fn needs_gfw_boot(&self) -> bool {
        true
    }
}

impl GpuHal for Ga102 {
    fn needs_gfw_boot(&self) -> bool {
        true
    }
}

impl GpuHal for Fsp {
    fn needs_gfw_boot(&self) -> bool {
        false
    }
}

const TU102: Tu102 = Tu102;
const GA100: Ga100 = Ga100;
const GA102: Ga102 = Ga102;
const FSP: Fsp = Fsp;

pub(super) fn gpu_hal(chipset: Chipset) -> &'static dyn GpuHal {
    match chipset.arch() {
        Architecture::Turing => &TU102,
        Architecture::Ampere if chipset == Chipset::GA100 => &GA100,
        Architecture::Ampere | Architecture::Ada => &GA102,
        Architecture::Hopper | Architecture::Blackwell => &FSP,
    }
}
