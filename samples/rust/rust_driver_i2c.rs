// SPDX-License-Identifier: GPL-2.0

//! Rust I2C driver sample.

use kernel::{acpi, c_str, device::Core, i2c, of, prelude::*, types::ARef};

struct SampleDriver {
    pdev: ARef<i2c::Device>,
}

kernel::acpi_device_table! {
    ACPI_TABLE,
    MODULE_ACPI_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(acpi::DeviceId::new(b"TST0001"), 0)]
}

kernel::i2c_device_table! {
    I2C_TABLE,
    MODULE_I2C_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(i2c::DeviceId::new(b"rust_driver_i2c"), 0)]
}

kernel::of_device_table! {
    OF_TABLE,
    MODULE_OF_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("test,rust_driver_i2c")), 0)]
}

impl i2c::Driver for SampleDriver {
    type IdInfo = u32;

    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);
    const I2C_ID_TABLE: Option<i2c::IdTable<Self::IdInfo>> = Some(&I2C_TABLE);
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(pdev: &i2c::Device<Core>, info: Option<&Self::IdInfo>) -> Result<Pin<KBox<Self>>> {
        let dev = pdev.as_ref();

        dev_dbg!(dev, "Probe Rust I2C driver sample.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info);
        }

        let drvdata = KBox::new(Self { pdev: pdev.into() }, GFP_KERNEL)?;

        Ok(drvdata.into())
    }
    fn shutdown(pdev: &i2c::Device<Core>) {
        dev_dbg!(pdev.as_ref(), "Shutdown Rust I2C driver sample.\n");
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        dev_dbg!(self.pdev.as_ref(), "Remove Rust I2C driver sample.\n");
    }
}

kernel::module_i2c_driver! {
    type: SampleDriver,
    name: "rust_driver_i2c",
    authors: ["Igor Korotin"],
    description: "Rust I2C driver",
    license: "GPL v2",
}
