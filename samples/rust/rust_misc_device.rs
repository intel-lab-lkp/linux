// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Rust misc device sample.

use kernel::{
    c_str,
    ioctl::_IO,
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
};

const RUST_MISC_DEV_HELLO: u32 = _IO('R' as u32, 9);

module! {
    type: RustMiscDeviceModule,
    name: "rust_misc_device",
    author: "Lee Jones",
    description: "Rust misc device sample",
    license: "GPL",
}

struct RustMiscDeviceModule {
    _miscdev: Pin<KBox<MiscDeviceRegistration<RustMiscDevice>>>,
}

impl kernel::Module for RustMiscDeviceModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Initialising Rust Misc Device Sample\n");

        let options = MiscDeviceOptions {
            name: c_str!("rust-misc-device"),
        };

        Ok(Self {
            _miscdev: KBox::pin_init(
                MiscDeviceRegistration::<RustMiscDevice>::register(options),
                GFP_KERNEL,
            )?,
        })
    }
}

struct RustMiscDevice;

#[vtable]
impl MiscDevice for RustMiscDevice {
    type Ptr = KBox<Self>;

    fn open() -> Result<KBox<Self>> {
        pr_info!("Opening Rust Misc Device Sample\n");

        Ok(KBox::new(RustMiscDevice, GFP_KERNEL)?)
    }

    fn ioctl(
        _device: <Self::Ptr as kernel::types::ForeignOwnable>::Borrowed<'_>,
        cmd: u32,
        _arg: usize,
    ) -> Result<isize> {
        pr_info!("IOCTLing Rust Misc Device Sample\n");

        match cmd {
            RUST_MISC_DEV_HELLO => pr_info!("Hello from the Rust Misc Device\n"),
            _ => {
                pr_err!("IOCTL not recognised: {}\n", cmd);
                return Err(ENOIOCTLCMD);
            }
        }

        Ok(0)
    }
}

impl Drop for RustMiscDevice {
    fn drop(&mut self) {
        pr_info!("Exiting the Rust Misc Device Sample\n");
    }
}
