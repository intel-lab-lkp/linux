// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Rahul Rameshbabu <sergeantsagara@protonmail.com>

//! Rust reference HID driver for Glorious Model O and O- mice.

use kernel::{self, bindings, device, hid, prelude::*};

struct GloriousRust;

kernel::hid_device_table!(
    HID_TABLE,
    MODULE_HID_TABLE,
    <GloriousRust as hid::Driver>::IdInfo,
    [(
        hid::DeviceId::new_usb(
            hid::Group::Generic,
            bindings::USB_VENDOR_ID_SINOWEALTH,
            bindings::USB_DEVICE_ID_GLORIOUS_MODEL_O,
        ),
        (),
    )]
);

#[vtable]
impl hid::Driver for GloriousRust {
    type IdInfo = ();
    const ID_TABLE: hid::IdTable<Self::IdInfo> = &HID_TABLE;

    /// Fix the Glorious Model O and O- consumer input report descriptor to use
    /// the variable and relative flag, while clearing the const flag.
    ///
    /// Without this fixup, inputs from the mice will be ignored.
    fn report_fixup<'a, 'b: 'a>(hdev: &hid::Device<device::Core>, rdesc: &'b mut [u8]) -> &'a [u8] {
        if rdesc.len() == 213
            && (rdesc[84] == 129 && rdesc[85] == 3)
            && (rdesc[112] == 129 && rdesc[113] == 3)
            && (rdesc[140] == 129 && rdesc[141] == 3)
        {
            dev_info!(
                hdev.as_ref(),
                "patching Glorious Model O consumer control report descriptor\n"
            );

            rdesc[85] = hid::MAIN_ITEM_VARIABLE | hid::MAIN_ITEM_RELATIVE;
            rdesc[113] = hid::MAIN_ITEM_VARIABLE | hid::MAIN_ITEM_RELATIVE;
            rdesc[141] = hid::MAIN_ITEM_VARIABLE | hid::MAIN_ITEM_RELATIVE;
        }

        rdesc
    }
}

kernel::module_hid_driver! {
    type: GloriousRust,
    name: "GloriousRust",
    authors: ["Rahul Rameshbabu <sergeantsagara@protonmail.com>"],
    description: "Rust reference HID driver for Glorious Model O and O- mice",
    license: "GPL",
}
