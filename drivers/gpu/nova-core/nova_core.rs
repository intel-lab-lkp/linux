// SPDX-License-Identifier: GPL-2.0

//! Nova Core GPU Driver

use kernel::{
    debugfs::{Dir, Directory},
    error::Error,
    pci,
    prelude::*,
    InPlaceModule, //
};

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
mod vbios;

pub(crate) const MODULE_NAME: &kernel::str::CStr = <LocalModule as kernel::ModuleMetadata>::NAME;

#[pin_data]
struct NovaCoreModule {
    #[pin]
    _driver: kernel::driver::Registration<pci::Adapter<driver::NovaCore>>,
    _debugfs_root: Dir,
}

impl InPlaceModule for NovaCoreModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        // Create the debugfs top-level directory.  Each GPU will create a subdirectory.
        let debugfs_root = Dir::new(kernel::c_str!("nova_core"));

        try_pin_init!(Self {
            _driver <- kernel::driver::Registration::new(MODULE_NAME, module),
            _debugfs_root: debugfs_root,
        })
    }
}

module! {
    type: NovaCoreModule,
    name: "NovaCore",
    authors: ["Danilo Krummrich"],
    description: "Nova Core GPU driver",
    license: "GPL v2",
}

kernel::module_firmware!(firmware::ModInfoBuilder);
