// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM subsystem abstractions.

pub mod device;
pub mod driver;
pub mod file;
pub mod gem;
pub mod ioctl;
pub mod jq;

pub use self::device::Device;
pub use self::driver::Driver;
pub use self::driver::DriverInfo;
pub use self::driver::Registration;
pub use self::file::File;
pub use self::jq::Jobqueue;

pub(crate) mod private {
    pub trait Sealed {}
}
