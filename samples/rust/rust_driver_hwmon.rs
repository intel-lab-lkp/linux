// SPDX-License-Identifier: GPL-2.0-only

//! Rust hwmon device sample.

use kernel::{faux, hwmon, prelude::*};

module! {
    type: SampleModule,
    name: "rust_hwmon_driver",
    authors: ["DonjuanPlatium"],
    description: "Rust hwmon device sample",
    license: "GPL",
}

struct SampleHwmon;

#[vtable]
impl hwmon::Driver for SampleHwmon {
    fn read(&self, _sensor: hwmon::SensorType, _attr: u32, _channel: u32) -> Result<c_long> {
        // Always return 25°C.
        Ok(25000)
    }

    fn is_visible(&self, _sensor: hwmon::SensorType, _attr: u32, _channel: u32) -> u16 {
        // All declared attributes are world-readable.
        0o444
    }
}

struct SampleModule {
    _hwmon: hwmon::Registration<SampleHwmon>,
    _faux: faux::Registration,
}

impl kernel::Module for SampleModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Initialising Rust Hwmon Sample\n");

        let faux = faux::Registration::new(c"rust-hwmon-sample-device", None)?;

        let hwmon = hwmon::Registration::new(faux.as_ref(), c"sample", SampleHwmon)?;

        pr_info!("Registered hwmon device\n");

        Ok(Self {
            _hwmon: hwmon,
            _faux: faux,
        })
    }
}
