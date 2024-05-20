// SPDX-License-Identifier: GPL-2.0

//! Nova GPU Driver

mod driver;
mod file;
mod gem;
mod gpu;

use kernel::prelude::module;

use driver::NovaModule;

module! {
    type: NovaModule,
    name: "Nova",
    author: "Danilo Krummrich",
    description: "Nova GPU driver",
    license: "GPL v2",
}
