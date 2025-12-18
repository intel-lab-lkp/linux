// SPDX-License-Identifier: GPL-2.0

//! Nova Core GPU Driver

use kernel::{error::Error, pci, prelude::*, InPlaceModule};
use pin_init::{PinInit, pinned_drop};

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

#[pin_data(PinnedDrop)]
struct NovaCoreModule {
    #[pin]
    _driver: kernel::driver::Registration<pci::Adapter<driver::NovaCore>>,
}

impl InPlaceModule for NovaCoreModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        pr_info!("NovaCore GPU driver loaded\n");
        try_pin_init!(Self {
            _driver <- kernel::driver::Registration::new(MODULE_NAME, module),
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
