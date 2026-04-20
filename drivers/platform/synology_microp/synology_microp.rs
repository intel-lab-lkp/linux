// SPDX-License-Identifier: GPL-2.0

//! Synology Microp driver

use kernel::{
    device,
    led::Color,
    of::{
        DeviceId,
        IdTable, //
    },
    of_device_table,
    prelude::*,
    serdev, //
};
use pin_init::pin_init_scope;

use crate::model::Model;

pub(crate) mod command;
mod led;
mod model;

kernel::module_serdev_device_driver! {
    type: SynologyMicropDriver,
    name: "synology_microp",
    authors: ["Markus Probst <markus.probst@posteo.de>"],
    description: "Synology Microp driver",
    license: "GPL v2",
}

#[rustfmt::skip]
of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    Model,
    [
        // apollolake
        (DeviceId::new(c"synology,ds918p-microp"), Model::new()),

        // evansport
        (DeviceId::new(c"synology,ds214play-microp"), Model::new()),

        // geminilakenk
        (DeviceId::new(c"synology,ds225p-microp"), Model::new().led_usb_copy()),
        (DeviceId::new(c"synology,ds425p-microp"), Model::new()),

        // pineview
        (DeviceId::new(c"synology,ds710p-microp"), Model::new().led_esata()),
        (DeviceId::new(c"synology,ds1010p-microp"), Model::new().led_alert(Color::Orange)),
        (DeviceId::new(c"synology,ds411p-microp"), Model::new()),

        // r1000
        (DeviceId::new(c"synology,ds923p-microp"), Model::new()),
        (DeviceId::new(c"synology,ds723p-microp"), Model::new()),
        (DeviceId::new(c"synology,ds1522p-microp"), Model::new()),
        (DeviceId::new(c"synology,rs422p-microp"), Model::new().led_power(Color::Green)),

        // r1000nk
        (DeviceId::new(c"synology,ds725p-microp"), Model::new()),

        // rtd1296
        (DeviceId::new(c"synology,ds118-microp"), Model::new()),

        // rtd1619b
        (DeviceId::new(c"synology,ds124-microp"), Model::new()),
        (DeviceId::new(c"synolody,ds223-microp"), Model::new().led_usb_copy()),
        (DeviceId::new(c"synology,ds223j-microp"), Model::new()),

        // v1000
        (DeviceId::new(c"synology,ds1823xsp-microp"), Model::new()),
        (DeviceId::new(c"synology,rs822p-microp"), Model::new().led_power(Color::Green)),
        (DeviceId::new(c"synology,rs1221p-microp"), Model::new().led_power(Color::Green)),
        (DeviceId::new(c"synology,rs1221rpp-microp"), Model::new().led_power(Color::Green)),

        // v1000nk
        (DeviceId::new(c"synology,ds925p-microp"), Model::new()),
        (DeviceId::new(c"synology,ds1525p-microp"), Model::new()),
        (DeviceId::new(c"synology,ds1825p-microp"), Model::new()),
    ]
);

#[pin_data]
struct SynologyMicropDriver {
    #[pin]
    led: led::Data,
}

#[vtable]
impl serdev::Driver for SynologyMicropDriver {
    type IdInfo = Model;
    const OF_ID_TABLE: Option<IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        dev: &serdev::Device<device::Core>,
        model: Option<&Model>,
    ) -> impl PinInit<Self, kernel::error::Error> {
        pin_init_scope(move || {
            let model = model.ok_or(EINVAL)?;

            dev.set_baudrate(9600).map_err(|_| EINVAL)?;
            dev.set_flow_control(false);
            dev.set_parity(serdev::Parity::None)?;

            Ok(try_pin_init!(Self {
                led <- led::Data::register(dev, model),
            }))
        })
    }
}
