// SPDX-License-Identifier: GPL-2.0

//! Rust GPIO consumer driver sample.

use kernel::{
    device::{
        self,
        Core, //
    },
    gpio::{
        self,
        consumer::{
            GpioDesc,
            GpiodFlags, //
        }, //
    },
    of,
    platform,
    prelude::*,
    time::{
        delay::fsleep,
        Delta, //
    }, //
};

struct SampleDriver;

impl platform::Driver for SampleDriver {
    type IdInfo = ();
    type Data<'bound> = Self;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self, Error> + 'bound {
        let dev = pdev.as_ref();

        dev_dbg!(dev, "Probe Rust GPIO consumer driver sample.\n");

        Self::examine_gpio(dev)?;

        Ok(Self)
    }
}

type GetValue = fn(&GpioDesc) -> Result<gpio::LogicalLineLevel>;
type GetRawValue = fn(&GpioDesc) -> Result<gpio::PhysicalLineLevel>;
type SetValue = fn(&GpioDesc, gpio::LogicalLineLevel) -> Result<()>;
type SetRawValue = fn(&GpioDesc, gpio::PhysicalLineLevel) -> Result<()>;

impl SampleDriver {
    fn examine_gpio(dev: &device::Device) -> Result {
        let gpiod = GpioDesc::get(dev, None, GpiodFlags::ASIS)?;

        let cansleep = gpiod.cansleep()?;
        let (get_value, get_raw_value, set_value, set_raw_value): (
            GetValue,
            GetRawValue,
            SetValue,
            SetRawValue,
        ) = if cansleep {
            (
                |gpiod| gpiod.get_value_cansleep(),
                |gpiod| gpiod.get_raw_value_cansleep(),
                |gpiod, value| gpiod.set_value_cansleep(value),
                |gpiod, value| gpiod.set_raw_value_cansleep(value),
            )
        } else {
            (
                |gpiod| gpiod.get_value(),
                |gpiod| gpiod.get_raw_value(),
                |gpiod, value| gpiod.set_value(value),
                |gpiod, value| gpiod.set_raw_value(value),
            )
        };

        let pr_status = || -> Result<()> {
            dev_info!(
                dev,
                "direction: {}, value: {}, raw value: {}\n",
                gpiod.get_direction()?,
                get_value(&gpiod)?,
                get_raw_value(&gpiod)?
            );
            Ok(())
        };

        let active_low = gpiod.is_active_low()?;

        dev_info!(
            dev,
            "got the GPIO ({}{})\n",
            if active_low {
                "active low"
            } else {
                "active high"
            },
            if cansleep { ", sleepy" } else { "" }
        );
        pr_status()?;

        gpiod.direction_output(gpio::LogicalLineLevel::Inactive)?;
        dev_info!(dev, "line is inactivated\n");
        pr_status()?;

        fsleep(Delta::from_millis(1));

        set_value(&gpiod, gpio::LogicalLineLevel::Active)?;
        dev_info!(dev, "line is activated\n");
        pr_status()?;

        fsleep(Delta::from_millis(1));

        set_value(&gpiod, gpio::LogicalLineLevel::Inactive)?;
        dev_info!(dev, "GPIO: line is inactivated\n");
        pr_status()?;

        fsleep(Delta::from_millis(1));

        let level = get_raw_value(&gpiod)?;

        gpiod.direction_input()?;
        dev_info!(dev, "line is input mode\n");
        pr_status()?;

        fsleep(Delta::from_millis(1));

        gpiod.direction_output_raw(!level)?;
        dev_info!(dev, "line is toggled\n");
        pr_status()?;

        fsleep(Delta::from_millis(1));

        set_raw_value(&gpiod, !!level)?;
        dev_info!(dev, "line is toggled\n");
        pr_status()?;

        Ok(())
    }
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SampleDriver as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c"test,rust-gpio-consumer"), ())]
);

kernel::module_platform_driver! {
    type: SampleDriver,
    name: "rust_gpio_consumer",
    authors: ["Kohei Ito"],
    description: "Rust GPIO consumer driver",
    license: "GPL v2",
}
