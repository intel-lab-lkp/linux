// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Rust misc device sample.
//!
//! Below is an example userspace C program that exercises this sample's functionality.
//!
//! ```c
//!#include <errno.h>
//!#include <fcntl.h>
//!#include <stdint.h>
//!#include <stdio.h>
//!#include <stdlib.h>
//!#include <sys/ioctl.h>
//!#include <unistd.h>
//!
//!#define RUST_MISC_DEV_FAIL _IO('|', 0)
//!#define RUST_MISC_DEV_HELLO _IO('|', 0x80)
//!#define RUST_MISC_DEV_GET_VALUE _IOR('|', 0x81, int)
//!#define RUST_MISC_DEV_SET_VALUE _IOW('|', 0x82, int)
//!
//!int main() {
//!  int value, new_value;
//!  int fd, ret;
//!
//!  // Open the device file
//!  printf("Opening /dev/rust-misc-device for reading and writing\n");
//!  fd = open("/dev/rust-misc-device", O_RDWR);
//!  if (fd < 0) {
//!    perror("open");
//!    return errno;
//!  }
//!
//!  // Make call into driver to say "hello"
//!  printf("Calling Hello\n");
//!  ret = ioctl(fd, RUST_MISC_DEV_HELLO, NULL);
//!  if (ret < 0) {
//!    perror("ioctl: Failed to call into Hello");
//!    close(fd);
//!    return errno;
//!  }
//!
//!  // Get initial value
//!  printf("Fetching initial value\n");
//!  ret = ioctl(fd, RUST_MISC_DEV_GET_VALUE, &value);
//!  if (ret < 0) {
//!    perror("ioctl: Failed to fetch the initial value");
//!    close(fd);
//!    return errno;
//!  }
//!
//!  value++;
//!
//!  // Set value to something different
//!  printf("Submitting new value (%d)\n", value);
//!  ret = ioctl(fd, RUST_MISC_DEV_SET_VALUE, &value);
//!  if (ret < 0) {
//!    perror("ioctl: Failed to submit new value");
//!    close(fd);
//!    return errno;
//!  }
//!
//!  // Ensure new value was applied
//!  printf("Fetching new value\n");
//!  ret = ioctl(fd, RUST_MISC_DEV_GET_VALUE, &new_value);
//!  if (ret < 0) {
//!    perror("ioctl: Failed to fetch the new value");
//!    close(fd);
//!    return errno;
//!  }
//!
//!  if (value != new_value) {
//!    printf("Failed: Committed and retrieved values are different (%d -%d)\n",
//!           value, new_value);
//!    close(fd);
//!    return -1;
//!  }
//!
//!  // Call the unsuccessful ioctl
//!  printf("Attempting to call in to an non-existent IOCTL\n");
//!  ret = ioctl(fd, RUST_MISC_DEV_FAIL, NULL);
//!  if (ret < 0) {
//!    perror("ioctl: Succeeded to fail - this was expected");
//!  } else {
//!    printf("ioctl: Failed to fail\n");
//!    close(fd);
//!    return -1;
//!  }
//!
//!  ssize_t bytes_written;
//!
//!  // Write
//!  char str1[] = {'H', 'e', 'l', 'l', 'o', '\0'};
//!  printf("Attempting to write to the file\n");
//!  bytes_written = write(fd, str1, sizeof(str1) * sizeof(uint8_t));
//!  if (bytes_written < 0) {
//!    perror("Error writing to file");
//!    close(fd);
//!    return 1;
//!  }
//!
//!  // Write with a custom offset
//!  printf("Attempting to write to the file with a custom offset\n");
//!  char str2[] = {'i', ',', ' ', 'w', 'o', 'r', 'l', 'd', '!', '\0'};
//!  // bytes_written = write(fd, str2, sizeof(str2) * sizeof(uint8_t));
//!  bytes_written = pwrite(fd, str2, sizeof(str2), 1);
//!  if (bytes_written < 0) {
//!    perror("Error writing to file 2");
//!    close(fd);
//!    return 1;
//!  }
//!
//!  // Read
//!  printf("Attempting to read from the file\n");
//!  char buffer1[20];
//!  ssize_t readCount1 = pread(fd, buffer1, sizeof(buffer1), 0);
//!  if (readCount1 < 0) {
//!    perror("Error reading from file\n");
//!    close(fd);
//!    return errno;
//!  }
//!  printf("Data: \"%s\n\"", buffer1);
//!
//!  // Read with a custom offset
//!  printf("Attempting to read from the file with a custom offset\n");
//!  char buffer2[20];
//!  ssize_t readCount2 = pread(fd, buffer2, sizeof(buffer2), 4);
//!  if (readCount2 < 0) {
//!    perror("Error reading from file\n");
//!    close(fd);
//!    return errno;
//!  }
//!  printf("Data: \"%s\n\"", buffer2);
//!
//!  // Close the device file
//!  printf("Closing /dev/rust-misc-device\n");
//!  close(fd);
//!
//!  printf("Success\n");
//!  return 0;
//!}
//! ```

use core::pin::Pin;

use kernel::{
    c_str,
    device::Device,
    fs::{file_operations::FileOperations, File},
    global_lock,
    ioctl::{_IO, _IOC_SIZE, _IOR, _IOW},
    miscdevice::{MiscDeviceOptions, MiscDeviceRegistration},
    new_mutex,
    prelude::*,
    sync::Mutex,
    types::{ARef, ForeignOwnable},
    uaccess::{UserSlice, UserSliceReader, UserSliceWriter},
};

const RUST_MISC_DEV_HELLO: u32 = _IO('|' as u32, 0x80);
const RUST_MISC_DEV_GET_VALUE: u32 = _IOR::<i32>('|' as u32, 0x81);
const RUST_MISC_DEV_SET_VALUE: u32 = _IOW::<i32>('|' as u32, 0x82);

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

global_lock! {
    // SAFETY: Initialized in module initializer before first use.
    unsafe(uninit) static DATA: Mutex<KVec<u8>> = KVec::new();
}

impl kernel::InPlaceModule for RustMiscDeviceModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        pr_info!("Initialising Rust Misc Device Sample\n");

        let options = MiscDeviceOptions {
            name: c_str!("rust-misc-device"),
        };

        // SAFETY: Called exactly once.
        unsafe { DATA.init() };

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
impl FileOperations for RustMiscDevice {
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
                dev_warn!(me.dev, "-> IOCTL not recognised: {}\n", cmd);
                return Err(ENOTTY);
            }
        };

        Ok(0)
    }

    fn read(
        device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        mut buf: UserSliceWriter,
        offset: &mut i64,
    ) -> Result<i64> {
        dev_info!(device.dev, "read() called\n");

        let data = DATA.lock();

        if *offset >= data.len() as i64 {
            return Ok(0);
        }

        let slice = &data[(*offset as usize)..];

        dev_info!(device.dev, "Writing to user: {:?}\n", slice);

        buf.write_slice(slice)?;
        *offset += slice.len() as i64;

        Ok(data.len() as i64)
    }

    fn write(
        device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        buf: UserSliceReader,
        offset: &mut i64,
    ) -> Result<i64> {
        dev_info!(device.dev, "write() called\n");

        let mut data = DATA.lock();

        let mut tmp = KVec::with_capacity(buf.len(), GFP_KERNEL)?;
        buf.read_all(&mut tmp, GFP_KERNEL)?;

        for (i, val) in tmp.iter().enumerate() {
            let idx = *offset as usize + i;
            if idx < data.len() {
                data[idx] = *val;
            } else {
                data.push(*val, GFP_KERNEL)?;
            }
        }

        *offset += tmp.len() as i64;

        dev_info!(device.dev, "Reading from user: {:?}\n", tmp);

        Ok(*offset)
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
