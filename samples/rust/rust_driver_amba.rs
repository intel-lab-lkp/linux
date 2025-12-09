// SPDX-License-Identifier: GPL-2.0

//! Rust AMBA driver sample.
//!
//! This demonstrates how to write a Rust AMBA driver.
//!
//! AMBA (Advanced Microcontroller Bus Architecture) is a bus protocol
//! used in ARM-based systems. This sample shows:
//!
//! - How to define an AMBA device ID table
//! - How to implement the AMBA driver trait
//! - How to access device resources (memory and IRQ)
//!
//! To test this driver, you need an AMBA device with a matching device ID.
//! For example, you can use QEMU with an ARM platform that has AMBA devices.

use kernel::{amba, device::Core, error::Error, prelude::*, sync::aref::ARef};

struct SampleDriver {
    adev: ARef<amba::Device>,
}

struct Info(u32);

kernel::amba_device_table!(
    AMBA_TABLE,
    MODULE_AMBA_TABLE,
    <SampleDriver as amba::Driver>::IdInfo,
    [
        // Example: Match device ID 0x00041031 with mask 0x000fffff
        // This is a common pattern for ARM PrimeCell devices
        (amba::DeviceId::new(0x00041031, 0x000fffff), Info(1)),
        // You can add more device IDs here
        // (amba::DeviceId::new(0x00041032, 0x000fffff), Info(2)),
    ]
);

impl amba::Driver for SampleDriver {
    type IdInfo = Info;
    const AMBA_ID_TABLE: Option<amba::IdTable<Self::IdInfo>> = Some(&AMBA_TABLE);

    fn probe(adev: &amba::Device<Core>, info: Option<&Self::IdInfo>) -> impl PinInit<Self, Error> {
        let dev = adev.as_ref();

        dev_dbg!(dev, "Probe Rust AMBA driver sample.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info.0);
        }

        // Access device resource
        if let Some(resource) = adev.resource() {
            let start = resource.start();
            let size = resource.size();
            dev_info!(
                dev,
                "Device resource: start={:#x}, size={:#x}\n",
                start,
                size
            );
        }

        // Try to access IRQ (if available)
        if let Ok(irq_request) = adev.irq_by_index(0) {
            dev_info!(dev, "IRQ available: {}\n", irq_request.irq());
        } else {
            dev_info!(dev, "No IRQ available at index 0\n");
        }

        Ok(Self { adev: adev.into() })
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        dev_dbg!(self.adev.as_ref(), "Remove Rust AMBA driver sample.\n");
    }
}

kernel::module_amba_driver! {
    type: SampleDriver,
    name: "rust_driver_amba",
    authors: ["Ke Sun"],
    description: "Rust AMBA driver sample",
    license: "GPL v2",
}
