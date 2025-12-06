// SPDX-License-Identifier: GPL-2.0
//
use kernel::{
    device,
    pci,
    prelude::*, //
};

use crate::{
    module_parameters, //
};

pub(crate) struct Vgpu {
    pub vgpu_support: bool,
}

impl Vgpu {
    pub(crate) fn new(pdev: &pci::Device<device::Bound>) -> Result<Vgpu> {
        Ok(Vgpu {
            vgpu_support: match *module_parameters::vgpu_support.value() {
                0 => false,
                _ => pdev.sriov_get_totalvfs().is_ok(),
            },
        })
    }
}
