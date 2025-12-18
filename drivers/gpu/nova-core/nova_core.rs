// SPDX-License-Identifier: GPL-2.0

//! Nova Core GPU Driver

use kernel::{debugfs::Dir, error::Error, pci, prelude::*, InPlaceModule};

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

pub(crate) const MODULE_NAME: &kernel::str::CStr = <LocalModule as kernel::ModuleMetadata>::NAME;

#[pin_data]
struct NovaCoreModule {
    // Note: field order matters for drop order. The driver must be dropped before
    // the debugfs directory so that device subdirectories are removed first.
    #[pin]
    _driver: kernel::driver::Registration<pci::Adapter<driver::NovaCore>>,
    _debugfs_root: Dir,
}

impl InPlaceModule for NovaCoreModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        pr_info!("NovaCore GPU driver loaded\n");

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
