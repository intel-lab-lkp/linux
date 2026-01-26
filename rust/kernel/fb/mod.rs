// SPDX-License-Identifier: GPL-2.0

//! Framebuffer subsystem.
//!
//! This module provides abstractions for the Linux framebuffer subsystem,
//! allowing drivers to be written in Rust.
//!
//! C headers:
//! - [`include/linux/fb.h`](srctree/include/linux/fb.h)

pub mod blit;
pub mod device;
pub mod driver;
pub mod io;
pub mod screeninfo;

pub use blit::{cfb_copyarea, cfb_fillrect, cfb_imageblit, CopyArea, FillRect, Image};
pub use device::Device;
pub use driver::{Driver, DriverInfo, Operations, Registration};
pub use io::{fb_io_mmap, fb_io_read, fb_io_write};
pub use screeninfo::{
    accel, activate, types, visual, vmode, Bitfield, FixScreenInfo, VarScreenInfo,
};
