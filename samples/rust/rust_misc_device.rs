// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Rust misc device sample.
//!
//! Below is an example userspace C program that exercises this sample's functionality.
//!
//! ```c
//! #include <stdio.h>
//! #include <stdlib.h>
//! #include <errno.h>
//! #include <fcntl.h>
//! #include <unistd.h>
//! #include <sys/ioctl.h>
//!
//! #define RUST_MISC_DEV_FAIL _IO('|', 0)
//! #define RUST_MISC_DEV_HELLO _IO('|', 0x80)
//! #define RUST_MISC_DEV_GET_VALUE _IOR('|', 0x81, int)
//! #define RUST_MISC_DEV_SET_VALUE _IOW('|', 0x82, int)
//!
//! int main() {
//!   int value, new_value;
//!   int fd, ret;
//!
//!   // Open the device file
//!   printf("Opening /dev/rust-misc-device for reading and writing\n");
//!   fd = open("/dev/rust-misc-device", O_RDWR);
//!   if (fd < 0) {
//!     perror("open");
//!     return errno;
//!   }
//!
//!   // Make call into driver to say "hello"
//!   printf("Calling Hello\n");
//!   ret = ioctl(fd, RUST_MISC_DEV_HELLO, NULL);
//!   if (ret < 0) {
//!     perror("ioctl: Failed to call into Hello");
//!     close(fd);
//!     return errno;
//!   }
//!
//!   // Get initial value
//!   printf("Fetching initial value\n");
//!   ret = ioctl(fd, RUST_MISC_DEV_GET_VALUE, &value);
//!   if (ret < 0) {
//!     perror("ioctl: Failed to fetch the initial value");
//!     close(fd);
//!     return errno;
//!   }
//!
//!   value++;
//!
//!   // Set value to something different
//!   printf("Submitting new value (%d)\n", value);
//!   ret = ioctl(fd, RUST_MISC_DEV_SET_VALUE, &value);
//!   if (ret < 0) {
//!     perror("ioctl: Failed to submit new value");
//!     close(fd);
//!     return errno;
//!   }
//!
//!   // Ensure new value was applied
//!   printf("Fetching new value\n");
//!   ret = ioctl(fd, RUST_MISC_DEV_GET_VALUE, &new_value);
//!   if (ret < 0) {
//!     perror("ioctl: Failed to fetch the new value");
//!     close(fd);
//!     return errno;
//!   }
//!
//!   if (value != new_value) {
//!     printf("Failed: Committed and retrieved values are different (%d - %d)\n", value, new_value);
//!     close(fd);
//!     return -1;
//!   }
//!
//!   // Call the unsuccessful ioctl
//!   printf("Attempting to call in to an non-existent IOCTL\n");
//!   ret = ioctl(fd, RUST_MISC_DEV_FAIL, NULL);
//!   if (ret < 0) {
//!     perror("ioctl: Succeeded to fail - this was expected");
//!   } else {
//!     printf("ioctl: Failed to fail\n");
//!     close(fd);
//!     return -1;
//!   }
//!
//!   // Set a file offset
//!   printf("Call lseek SEEK_SET\n");
//!   ret = lseek(fd, 10, SEEK_SET);
//!   if (ret == 10)
//!     printf("lseek: Succeed to SEEK_SET\n");
//!   else
//!     printf("lseek: Failed to SEEK_SET\n");
//!
//!   // Change the file offset from the initial value
//!   printf("Call lseek SEEK_CUR\n");
//!   ret = lseek(fd, 10, SEEK_CUR);
//!   if (ret == 20)
//!     printf("lseek: Succeed to SEEK_CUR\n");
//!   else
//!     printf("lseek: Failed to SEEK_CUR\n");
//!
//!   // i_size is 0. So the following task always should fail.
//!   printf("Call lseek SEEK_END\n");
//!   ret = lseek(fd, -10, SEEK_END);
//!   if (ret < 0)
//!     perror("lseek: Succeeded to fail - this was expected");
//!   else {
//!     printf("lseek: Failed to fail SEEK_END\n");
//!     close(fd);
//!     return -1;
//!   }
//!
//!   // Close the device file
//!   printf("Closing /dev/rust-misc-device\n");
//!   close(fd);
//!
//!   printf("Success\n");
//!   return 0;
//! }
//! ```

use core::pin::Pin;

use kernel::{
    c_str,
    device::Device,
    fs::File,
    ioctl::{_IO, _IOC_SIZE, _IOR, _IOW},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    new_mutex,
    prelude::*,
    sync::Mutex,
    types::ARef,
    uaccess::{UserSlice, UserSliceReader, UserSliceWriter},
};

const RUST_MISC_DEV_HELLO: u32 = _IO('|' as u32, 0x80);
const RUST_MISC_DEV_GET_VALUE: u32 = _IOR::<i32>('|' as u32, 0x81);
const RUST_MISC_DEV_SET_VALUE: u32 = _IOW::<i32>('|' as u32, 0x82);

const SEEK_SET: i32 = 0;
const SEEK_CUR: i32 = 1;
const SEEK_END: i32 = 2;

module! {
    type: RustMiscDeviceModule,
    name: "rust_misc_device",
    authors: ["Lee Jones"],
    description: "Rust misc device sample",
    license: "GPL",
}

#[pin_data]
struct RustMiscDeviceModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<RustMiscDevice>,
}

impl kernel::InPlaceModule for RustMiscDeviceModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        pr_info!("Initialising Rust Misc Device Sample\n");

        let options = MiscDeviceOptions {
            name: c_str!("rust-misc-device"),
        };

        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(options),
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
    dev: ARef<Device>,
}

#[vtable]
impl MiscDevice for RustMiscDevice {
    type Ptr = Pin<KBox<Self>>;

    fn open(_file: &File, misc: &MiscDeviceRegistration<Self>) -> Result<Pin<KBox<Self>>> {
        let dev = ARef::from(misc.device());

        dev_info!(dev, "Opening Rust Misc Device Sample\n");

        KBox::try_pin_init(
            try_pin_init! {
                RustMiscDevice {
                    inner <- new_mutex!( Inner{ value: 0_i32 } ),
                    dev: dev,
                }
            },
            GFP_KERNEL,
        )
    }

    fn llseek(me: Pin<&RustMiscDevice>, file: &File, offset: i64, whence: i32) -> Result<isize> {
        dev_info!(me.dev, "LLSEEK Rust Misc Device Sample\n");
        let pos: i64;
        let eof: i64;

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * f_inode must be valid while the file is valid.
        unsafe {
            pos = (*file.as_ptr()).f_pos;
            eof = (*(*file.as_ptr()).f_inode).i_size;
        }

        let new_pos = match whence {
            SEEK_SET => offset,
            SEEK_CUR => pos + offset,
            SEEK_END => eof + offset,
            _ => {
                dev_err!(me.dev, "LLSEEK does not recognised: {}.\n", whence);
                return Err(EINVAL);
            }
        };

        if new_pos < 0 {
            dev_err!(me.dev, "The file offset becomes negative: {}.\n", new_pos);
            return Err(EINVAL);
        }

        // SAFETY: The file is valid for the duration of this call.
        let ret: isize = unsafe {
            (*file.as_ptr()).f_pos = new_pos;
            new_pos as isize
        };

        Ok(ret)
    }

    fn ioctl(me: Pin<&RustMiscDevice>, _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        dev_info!(me.dev, "IOCTLing Rust Misc Device Sample\n");

        // Treat the ioctl argument as a user pointer.
        let arg = UserPtr::from_addr(arg);
        let size = _IOC_SIZE(cmd);

        match cmd {
            RUST_MISC_DEV_GET_VALUE => me.get_value(UserSlice::new(arg, size).writer())?,
            RUST_MISC_DEV_SET_VALUE => me.set_value(UserSlice::new(arg, size).reader())?,
            RUST_MISC_DEV_HELLO => me.hello()?,
            _ => {
                dev_err!(me.dev, "-> IOCTL not recognised: {}\n", cmd);
                return Err(ENOTTY);
            }
        };

        Ok(0)
    }
}

#[pinned_drop]
impl PinnedDrop for RustMiscDevice {
    fn drop(self: Pin<&mut Self>) {
        dev_info!(self.dev, "Exiting the Rust Misc Device Sample\n");
    }
}

impl RustMiscDevice {
    fn set_value(&self, mut reader: UserSliceReader) -> Result<isize> {
        let new_value = reader.read::<i32>()?;
        let mut guard = self.inner.lock();

        dev_info!(
            self.dev,
            "-> Copying data from userspace (value: {})\n",
            new_value
        );

        guard.value = new_value;
        Ok(0)
    }

    fn get_value(&self, mut writer: UserSliceWriter) -> Result<isize> {
        let guard = self.inner.lock();
        let value = guard.value;

        // Free-up the lock and use our locally cached instance from here
        drop(guard);

        dev_info!(
            self.dev,
            "-> Copying data to userspace (value: {})\n",
            &value
        );

        writer.write::<i32>(&value)?;
        Ok(0)
    }

    fn hello(&self) -> Result<isize> {
        dev_info!(self.dev, "-> Hello from the Rust Misc Device\n");

        Ok(0)
    }
}
