// SPDX-License-Identifier: GPL-2.0

//! Rust USB sample.

use kernel::prelude::*;

module! {
    type: UsbSimple,
    name: "rust_usb_simple",
    author: "Martin Rodriguez Reboredo",
    description: "Rust USB sample",
    license: "GPL v2",
}

struct UsbSimple;

impl kernel::Module for UsbSimple {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("usb enabled: {}", !usb::disabled());
        Ok(UsbSimple)
    }
}
