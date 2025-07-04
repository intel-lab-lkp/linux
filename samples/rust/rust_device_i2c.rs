// SPDX-License-Identifier: GPL-2.0

//! Rust I2C DeviceOwned usage sample.
//!
//! This sample driver manually creates i2c_client using I2C board info
//! and pointer to I2C Adapter structure.
//!
//! For reproduction of the scenario one should compile kernel with i2c-dev and i2c-stub
//! modules enabled. f

use kernel::{c_str, device::Core, i2c, prelude::*};

struct SampleDriver {
    _owned: i2c::DeviceOwned<Core>,
}

// SAFETY: SampleDriver contains only one field `owned: DeviceOwned<Core>`,
// which is initialized in `init()` and dropped on module unload.
// There is no interior mutability or concurrent access to its contents
// (all I²C operations happen in single-threaded init/drop contexts),
// so it is safe to share &SampleDriver across threads.
unsafe impl Sync for SampleDriver {}

const BOARD_INFO: i2c::I2cBoardInfo = i2c::I2cBoardInfo::new(c_str!("rust_driver_i2c"), 0x30);

impl kernel::Module for SampleDriver {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_debug!("Probe Rust I2C device sample.\n");

        let adapter = i2c::I2cAdapterRef::get(0).ok_or(EINVAL)?;

        let device = i2c::DeviceOwned::<Core>::new(&adapter, &BOARD_INFO).ok_or(EINVAL)?;

        Ok(Self { _owned: device })
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        pr_debug!("Drop Rust I2C device sample.\n");
    }
}

kernel::prelude::module! {
    type:SampleDriver,
    name:"rust_device_i2c",
    authors:["Igor Korotin"],
    description:"Rust I2C device manual creation driver ",
    license:"GPL v2",
}
