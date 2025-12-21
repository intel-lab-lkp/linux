//! This is SAMPLE DRIVER
//! It doesn't aim into upstream and serves only demonstration purposes for
//! RFC patchset

use kernel::sync::atomic::{Atomic, Relaxed};

use kernel::{
    acpi::AcpiObject,
    device, module_wmi_driver, pr_info,
    prelude::*,
    wmi::{self, Device, DeviceId, Driver},
    wmi_device_table,
};

wmi_device_table!(
    REDMI_TABLE,
    MODULE_REDMI_TABLE,
    <RedmiWMIDriver as Driver>::IdInfo,
    [(DeviceId::new(b"46C93E13-EE9B-4262-8488-563BCA757FEF"), 12)]
);

struct RedmiWMIDriver {
    probed_val: i32,
    invokation_cnt: Atomic<i64>,
}

#[vtable]
impl wmi::Driver for RedmiWMIDriver {
    type IdInfo = i32;

    const TABLE: &'static dyn kernel::device_id::IdTable<DeviceId, Self::IdInfo> = &REDMI_TABLE;

    fn probe(
        _: &Device<kernel::device::Core>,
        id_info: &Self::IdInfo,
    ) -> impl PinInit<Self, Error> {
        pr_info!("Rust WMI Sample Driver probed with val {id_info}\n");

        Ok(Self {
            probed_val: *id_info,
            invokation_cnt: Atomic::new(0),
        })
    }

    fn notify(&self, _: &Device<device::Core>, _: &AcpiObject) {
        pr_info!(
            "Notified driver with probed_val: {}, invokation cnt: {}",
            self.probed_val,
            self.invokation_cnt.fetch_add(1, Relaxed)
        );
    }
}

module_wmi_driver!(
    type: RedmiWMIDriver,
    name: "redmi_wmi_sample",
    authors: ["Gladyshev Ilya"],
    description: "SAMPLE DRIVER for RFC demonstration only",
    license: "GPL v2",
);
