// SPDX-License-Identifier: GPL-2.0+
// Copyright (C) 2026 Artem Lytkin <iprintercanon@gmail.com>

//! Rust LSI ET1011C PHY driver
//!
//! C version of this driver: [`drivers/net/phy/et1011c.c`](./et1011c.c)

use kernel::{
    net::phy::{self, reg::C22, DeviceId, Driver},
    prelude::*,
};

kernel::module_phy_driver! {
    drivers: [PhyET1011C],
    device_table: [
        DeviceId::new_with_driver::<PhyET1011C>()
    ],
    name: "rust_et1011c_phy",
    authors: ["Artem Lytkin <iprintercanon@gmail.com>"],
    description: "Rust LSI ET1011C PHY driver",
    license: "GPL",
}

// Vendor-specific registers
const ET1011C_STATUS_REG: C22 = C22::vendor_specific::<0x1A>();
const ET1011C_CONFIG_REG: C22 = C22::vendor_specific::<0x16>();

// ET1011C status register fields
const ET1011C_SPEED_MASK: u16 = 0x0300;
const ET1011C_GIGABIT_SPEED: u16 = 0x0200;

// ET1011C config register fields
const ET1011C_TX_FIFO_MASK: u16 = 0x3000;
const ET1011C_TX_FIFO_DEPTH_16: u16 = 0x1000;
const ET1011C_GMII_INTERFACE: u16 = 0x0002;
const ET1011C_SYS_CLK_EN: u16 = 0x0010;

struct PhyET1011C;

#[vtable]
impl Driver for PhyET1011C {
    const NAME: &'static CStr = c"ET1011C";
    const PHY_DEVICE_ID: DeviceId = DeviceId::new_with_model_mask(0x0282f014);

    fn soft_reset(dev: &mut phy::Device) -> Result {
        dev.genphy_soft_reset()
    }

    fn read_status(dev: &mut phy::Device) -> Result<u16> {
        let old_speed = dev.speed();
        dev.genphy_read_status::<C22>()?;

        if old_speed != dev.speed() {
            let val = dev.read(ET1011C_STATUS_REG)?;
            if (val & ET1011C_SPEED_MASK) == ET1011C_GIGABIT_SPEED {
                let cfg = dev.read(ET1011C_CONFIG_REG)?;
                let cfg = cfg & !ET1011C_TX_FIFO_MASK;
                dev.write(
                    ET1011C_CONFIG_REG,
                    cfg | ET1011C_GMII_INTERFACE
                        | ET1011C_SYS_CLK_EN
                        | ET1011C_TX_FIFO_DEPTH_16,
                )?;
            }
        }

        Ok(0)
    }
}
