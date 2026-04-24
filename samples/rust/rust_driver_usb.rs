// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2025 Collabora Ltd.

//! Rust USB driver sample.

use kernel::{
    device::{
        self,
        Core, //
    },
    prelude::*,
    sync::aref::ARef,
    usb, //
};

struct SampleDriver {
    _intf: ARef<usb::Interface>,
}

kernel::usb_device_table!(
    USB_TABLE,
    MODULE_USB_TABLE,
    <SampleDriver as usb::Driver<'_>>::IdInfo,
    [(usb::DeviceId::from_id(0x1234, 0x5678), ()),]
);

impl<'a> usb::Driver<'a> for SampleDriver {
    type IdInfo = ();
    const ID_TABLE: usb::IdTable<Self::IdInfo> = &USB_TABLE;

    fn probe(
        intf: &'a usb::Interface<Core>,
        _id: &'a usb::DeviceId,
        _info: &'a Self::IdInfo,
    ) -> impl PinInit<Self, Error> + 'a {
        let dev: &device::Device<Core> = intf.as_ref();
        dev_info!(dev, "Rust USB driver sample probed\n");

        Ok(Self { _intf: intf.into() })
    }

    fn disconnect(intf: &'a usb::Interface<Core>, _data: Pin<&'a Self>) {
        let dev: &device::Device<Core> = intf.as_ref();
        dev_info!(dev, "Rust USB driver sample disconnected\n");
    }
}

kernel::module_usb_driver! {
    type: SampleDriver,
    name: "rust_driver_usb",
    authors: ["Daniel Almeida"],
    description: "Rust USB driver sample",
    license: "GPL v2",
}
