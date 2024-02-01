// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024 Christina Quast <contact@christina-quast.de>

//! Rust Rockchip PHY driver
//!
//! C version of this driver: [`drivers/net/phy/rockchip.c`](./rockchip.c)
use kernel::{
    c_str,
    net::phy::{self, DeviceId, Driver},
    prelude::*,
    uapi,
};

kernel::module_phy_driver! {
    drivers: [PhyRockchip],
    device_table: [
        DeviceId::new_with_driver::<PhyRockchip>(),
    ],
    name: "rust_asix_phy",
    author: "FUJITA Tomonori <fujita.tomonori@gmail.com>",
    description: "Rust Asix PHYs driver",
    license: "GPL",
}


const MII_INTERNAL_CTRL_STATUS: u16 = 17;
const SMI_ADDR_TSTCNTL: u16 = 20;
const SMI_ADDR_TSTWRITE: u16 = 23;

const MII_AUTO_MDIX_EN: u16 = bit(7);
const MII_MDIX_EN: u16 = bit(6);

const TSTCNTL_WR: u16 = bit(14) | bit(10);

const TSTMODE_ENABLE: u16 = 0x400;
const TSTMODE_DISABLE: u16 = 0x0;

const WR_ADDR_A7CFG: u16 = 0x18;

struct PhyRockchip;

impl PhyRockchip {
   /// Helper function for helper_integrated_phy_analog_init
    fn helper_init_tstmode(dev: &mut phy::Device) -> Result {
        // Enable access to Analog and DSP register banks
        dev.write(SMI_ADDR_TSTCNTL, TSTMODE_ENABLE)?;
        dev.write(SMI_ADDR_TSTCNTL, TSTMODE_DISABLE)?;
        dev.write(SMI_ADDR_TSTCNTL, TSTMODE_ENABLE)
    }

    /// Helper function for helper_integrated_phy_analog_init
    fn helper_close_tstmode(dev: &mut phy::Device) -> Result {
        dev.write(SMI_ADDR_TSTCNTL, TSTMODE_DISABLE)
    }

    /// Helper function for rockchip_config_init
    fn helper_integrated_phy_analog_init(dev: &mut phy::Device) -> Result {
        Self::helper_init_tstmode(dev)?;
        dev.write(SMI_ADDR_TSTWRITE, 0xB)?;
        dev.write(SMI_ADDR_TSTCNTL, TSTCNTL_WR | WR_ADDR_A7CFG)?;
        Self::helper_close_tstmode(dev)
    }

    /// Helper function for config_init
    fn helper_config_init(dev: &mut phy::Device) -> Result {
        let val = !MII_AUTO_MDIX_EN & dev.read(MII_INTERNAL_CTRL_STATUS)?;
        dev.write(MII_INTERNAL_CTRL_STATUS, val)?;
        Self::helper_integrated_phy_analog_init(dev)
    }

    fn helper_set_polarity(dev: &mut phy::Device, polarity: u8) -> Result {
        let reg = !MII_AUTO_MDIX_EN & dev.read(MII_INTERNAL_CTRL_STATUS)?;
        let val = match polarity as u32 {
            // status: MDI; control: force MDI
            uapi::ETH_TP_MDI => Some(reg & !MII_MDIX_EN),
            // status: MDI-X; control: force MDI-X
            uapi::ETH_TP_MDI_X => Some(reg | MII_MDIX_EN),
            // uapi::ETH_TP_MDI_AUTO => control: auto-select
            // uapi::ETH_TP_MDI_INVALID => status: unknown; control: unsupported
            _ => None,
        };
        if let Some(v) = val {
            if v != reg {
                return dev.write(MII_INTERNAL_CTRL_STATUS, v);
            }
        }
        Ok(())

    }
}

#[vtable]
impl Driver for PhyRockchip {
    const FLAGS: u32 = 0;
    const NAME: &'static CStr = c_str!("Rockchip integrated EPHY");
    const PHY_DEVICE_ID: DeviceId = DeviceId::new_with_custom_mask(0x1234d400, 0xfffffff0);

    fn link_change_notify(dev: &mut phy::Device) {
    // If mode switch happens from 10BT to 100BT, all DSP/AFE
    // registers are set to default values. So any AFE/DSP
    // registers have to be re-initialized in this case.
        if dev.state() == phy::DeviceState::Running && dev.speed() == uapi::SPEED_100 {
            if let Err(e) = Self::helper_integrated_phy_analog_init(dev) {
                pr_err!("rockchip: integrated_phy_analog_init err: {:?}", e);
            }
        }
    }

    fn soft_reset(dev: &mut phy::Device) -> Result {
        dev.genphy_soft_reset()
    }

    fn config_init(dev: &mut phy::Device) -> Result {
        PhyRockchip::helper_config_init(dev)
    }

    fn config_aneg(dev: &mut phy::Device) -> Result {
        PhyRockchip::helper_set_polarity(dev, dev.mdix())?;
        dev.genphy_config_aneg()
    }

    fn suspend(dev: &mut phy::Device) -> Result {
        dev.genphy_suspend()
    }

    fn resume(dev: &mut phy::Device) -> Result {
        let _ = dev.genphy_resume();

        PhyRockchip::helper_config_init(dev)
    }
}
