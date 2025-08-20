// SPDX-License-Identifier: GPL-2.0

//! Rust I2C driver sample.
//!
//! This module shows how to:
//!
//! 1. Manually create an `i2c_client` at address `SAMPLE_I2C_CLIENT_ADDR`
//!    on the adapter with index `SAMPLE_I2C_ADAPTER_INDEX`.
//! 2. Register a matching Rust-based I2C driver for that client.
//!
//! # Requirements
//!
//! - The target system must expose an I2C adapter at index
//!   `SAMPLE_I2C_ADAPTER_INDEX`.
//! - To emulate an adapter for testing, you can load the
//!   `i2c-stub` kernel module with an option `chip_addr`
//!   For example for this sample driver to emulate an I2C device with
//!   an address 0x30 you can use:
//!      `modprobe i2c-stub chip_addr=0x30`
//!

use kernel::{
    acpi, c_str,
    device::{Core, Normal},
    i2c, of,
    prelude::*,
    types::ARef,
};

const SAMPLE_I2C_CLIENT_ADDR: u16 = 0x30;
const SAMPLE_I2C_ADAPTER_INDEX: i32 = 0;
const BOARD_INFO: i2c::I2cBoardInfo =
    i2c::I2cBoardInfo::new(c_str!("rust_driver_i2c"), SAMPLE_I2C_CLIENT_ADDR);

struct SampleDriver {
    pdev: ARef<i2c::I2cClient>,
}

kernel::acpi_device_table! {
    ACPI_TABLE,
    MODULE_ACPI_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(acpi::DeviceId::new(c_str!("LNUXBEEF")), 0)]
}

kernel::i2c_device_table! {
    I2C_TABLE,
    MODULE_I2C_TABLE,
    <SampleDriver as i2c::Driver>::IdInfo,
    [(i2c::DeviceId::new(c_str!("rust_driver_i2c")), 0)]
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

    fn probe(pdev: &i2c::I2cClient<Core>, info: Option<&Self::IdInfo>) -> Result<Pin<KBox<Self>>> {
        let dev = pdev.as_ref();

        dev_dbg!(dev, "Probe Rust I2C driver sample.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info);
        }

        let drvdata = KBox::new(Self { pdev: pdev.into() }, GFP_KERNEL)?;

        Ok(drvdata.into())
    }

    fn shutdown(pdev: &i2c::I2cClient<Core>) {
        dev_dbg!(pdev.as_ref(), "Shutdown Rust I2C driver sample.\n");
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        dev_dbg!(self.pdev.as_ref(), "Remove Rust I2C driver sample.\n");
    }
}

// NOTE: The code below is expanded macro module_i2c_driver. It is not used here
//       because we need to manually create an I2C client in `init()`. The macro
//       hides `init()`, so to demo client creation on adapter SAMPLE_I2C_ADAPTER_INDEX
//       we expand it by hand.
type Ops<T> = kernel::i2c::Adapter<T>;

#[pin_data]
struct DriverModule {
    #[pin]
    _driver: kernel::driver::Registration<Ops<SampleDriver>>,
    _reg: i2c::Registration,
}

impl kernel::InPlaceModule for DriverModule {
    fn init(
        module: &'static kernel::ThisModule,
    ) -> impl ::pin_init::PinInit<Self, kernel::error::Error> {
        kernel::try_pin_init!(Self {
            _reg <- {
                let adapter = i2c::I2cAdapter::<Normal>::get(SAMPLE_I2C_ADAPTER_INDEX)?;

                i2c::Registration::new(adapter, &BOARD_INFO)
            },
            _driver <- kernel::driver::Registration::new(
                 <Self as kernel::ModuleMetadata>::NAME, module
            ),
        })
    }
}

kernel::prelude::module! {
    type: DriverModule,
    name: "rust_driver_i2c",
    authors: ["Igor Korotin"],
    description: "Rust I2C driver",
    license: "GPL v2",
}
