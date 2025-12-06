// SPDX-License-Identifier: GPL-2.0

//! Nova Core GPU Driver

#[macro_use]
mod bitfield;

mod dma;
mod driver;
mod falcon;
mod fb;
mod firmware;
mod gfw;
mod gpu;
mod gsp;
mod num;
mod regs;
mod sbuffer;
mod util;
mod vbios;
mod vgpu;

pub(crate) const MODULE_NAME: &kernel::str::CStr = <LocalModule as kernel::ModuleMetadata>::NAME;

kernel::module_pci_driver! {
    type: driver::NovaCore,
    name: "NovaCore",
    authors: ["Danilo Krummrich"],
    description: "Nova Core GPU driver",
    license: "GPL v2",
    firmware: [],
    params: {
        // vgpu_support = 1 (default): automatic
        //
        // The driver automatically enables or disables vGPU support based on if the GPU
        // advertises SRIOV caps.
        //
        // vgpu_support = 0: disabled
        //
        // Explicitly disables vGPU support. The driver will not enable vGPU support regardless.
        vgpu_support: u32 {
            default: 1,
            description: "Enable vGPU support - (1 = auto (default), 0 = disable)",
        },
    },
}

kernel::module_firmware!(firmware::ModInfoBuilder);
