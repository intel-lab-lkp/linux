// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Rust misc device sample.

use core::pin::Pin;

use kernel::{
    c_str,
    ioctl::{_IO, _IOC_SIZE, _IOR, _IOW},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    new_mutex,
    prelude::*,
    sync::Mutex,
    uaccess::{UserSlice, UserSliceReader, UserSliceWriter},
};

const RUST_MISC_DEV_GET_VALUE: u32 = _IOR::<i32>('R' as u32, 7);
const RUST_MISC_DEV_SET_VALUE: u32 = _IOW::<i32>('R' as u32, 8);
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

struct Inner {
    value: i32,
}

#[pin_data(PinnedDrop)]
struct RustMiscDevice {
    #[pin]
    inner: Mutex<Inner>,
}

#[vtable]
impl MiscDevice for RustMiscDevice {
    type Ptr = Pin<KBox<Self>>;

    fn open() -> Result<Pin<KBox<Self>>> {
        pr_info!("Opening Rust Misc Device Sample\n");

        KBox::try_pin_init(
            try_pin_init! {
                RustMiscDevice { inner <- new_mutex!( Inner{ value: 0_i32 } )}
            },
            GFP_KERNEL,
        )
    }

    fn ioctl(device: Pin<&RustMiscDevice>, cmd: u32, arg: usize) -> Result<isize> {
        pr_info!("IOCTLing Rust Misc Device Sample\n");

        let size = _IOC_SIZE(cmd);

        match cmd {
            RUST_MISC_DEV_GET_VALUE => device.get_value(UserSlice::new(arg, size).writer())?,
            RUST_MISC_DEV_SET_VALUE => device.set_value(UserSlice::new(arg, size).reader())?,
            RUST_MISC_DEV_HELLO => device.hello()?,
            _ => {
                pr_err!("-> IOCTL not recognised: {}\n", cmd);
                return Err(ENOIOCTLCMD);
            }
        };

        Ok(0)
    }
}

#[pinned_drop]
impl PinnedDrop for RustMiscDevice {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("Exiting the Rust Misc Device Sample\n");
    }
}

impl RustMiscDevice {
    fn set_value(&self, mut reader: UserSliceReader) -> Result<isize> {
        let new_value = reader.read::<i32>()?;
        let mut guard = self.inner.lock();

        pr_info!("-> Copying data from userspace (value: {})\n", new_value);

        guard.value = new_value;
        Ok(0)
    }

    fn get_value(&self, mut writer: UserSliceWriter) -> Result<isize> {
        let guard = self.inner.lock();
        let value = guard.value;

        // Refrain from calling write() on a locked resource
        drop(guard);

        pr_info!("-> Copying data to userspace (value: {})\n", &value);

        writer.write::<i32>(&value)?;
        Ok(0)
    }

    fn hello(&self) -> Result<isize> {
        pr_info!("-> Hello from the Rust Misc Device\n");

        Ok(0)
    }
}
