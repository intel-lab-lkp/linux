// SPDX-License-Identifier: GPL-2.0

//! Rust I2C driver sample.

use kernel::{
    acpi,
    device::Core,
    i2c,
    of,
    prelude::*, //
};

struct SampleDriver;

struct SampleDriverData<'bound> {
    idev: &'bound i2c::I2cClient,
}

kernel::acpi_device_table! {
    ACPI_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(acpi::DeviceId::new(c"LNUXBEEF"), 0)]
}

kernel::i2c_device_table! {
    I2C_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(i2c::DeviceId::new(c"rust_driver_i2c"), 0)]
}

kernel::of_device_table! {
    OF_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(of::DeviceId::new(c"test,rust_driver_i2c"), 0)]
}

impl i2c::Driver for SampleDriver {
    type IdInfo = u32;
    type Data<'bound> = SampleDriverData<'bound>;

    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);
    const I2C_ID_TABLE: Option<i2c::IdTable<Self::IdInfo>> = Some(&I2C_TABLE);
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        idev: &'bound i2c::I2cClient<Core<'_>>,
        info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        let dev = idev.as_ref();

        dev_info!(dev, "Probe Rust I2C driver sample.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info);
        }

        Ok(SampleDriverData { idev })
    }

    fn shutdown<'bound>(idev: &'bound i2c::I2cClient<Core<'_>>, _this: Pin<&Self::Data<'bound>>) {
        dev_info!(idev.as_ref(), "Shutdown Rust I2C driver sample.\n");
    }
}

impl Drop for SampleDriverData<'_> {
    fn drop(&mut self) {
        dev_info!(self.idev, "Unbind Rust I2C driver sample.\n");
    }
}

kernel::module_i2c_driver! {
    type: SampleDriver,
    name: "rust_driver_i2c",
    authors: ["Igor Korotin"],
    description: "Rust I2C driver",
    license: "GPL v2",
}
