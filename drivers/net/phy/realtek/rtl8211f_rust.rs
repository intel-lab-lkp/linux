// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026 Artem Lytkin <iprintercanon@gmail.com>

//! Rust Realtek RTL8211F Gigabit Ethernet PHY driver.

use kernel::irq::IrqReturn;
use kernel::net::phy::{self, reg::C22, DeviceId};
use kernel::prelude::*;

// Page select register (vendor-specific, register 0x1f).
const PAGE_SELECT: C22 = C22::vendor_specific::<0x1f>();

// Interrupt status register (vendor-specific, register 0x1d).
const INSR: C22 = C22::vendor_specific::<0x1d>();

// Interrupt enable register number (used with paged access on page 0xa42).
const INER_REG: u16 = 0x12;
const INER_LINK_STATUS: u16 = 1 << 4;

// RGMII TX delay register on page 0xd08.
const TX_DELAY_REG: u16 = 0x11;
const TX_DELAY_EN: u16 = 1 << 8;

// RGMII RX delay register on page 0xd08.
const RX_DELAY_REG: u16 = 0x15;
const RX_DELAY_EN: u16 = 1 << 3;

// PHY EEE enable bit in PHYCR2 (default page, register 0x19).
const PHYCR2: u16 = 0x19;
const PHYCR2_PHY_EEE_ENABLE: u16 = 1 << 5;

kernel::module_phy_driver! {
    drivers: [Rtl8211f],
    device_table: [
        DeviceId::new_with_driver::<Rtl8211f>(),
    ],
    name: "rtl8211f_rust",
    authors: ["Artem Lytkin"],
    description: "Rust Realtek RTL8211F PHY driver",
    license: "GPL",
}

struct Rtl8211f;

#[vtable]
impl phy::Driver for Rtl8211f {
    const NAME: &'static CStr = c"RTL8211F Gigabit Ethernet (Rust)";
    const PHY_DEVICE_ID: DeviceId = DeviceId::new_with_exact_mask(0x001cc916);

    fn config_init(dev: &mut phy::Device) -> Result {
        // Configure RGMII delays based on the PHY interface mode.
        // This matches the C driver's rtl8211f_config_rgmii_delay().
        let iface = dev.interface();
        let tx_delay = iface == bindings::phy_interface_t_PHY_INTERFACE_MODE_RGMII_ID
            || iface == bindings::phy_interface_t_PHY_INTERFACE_MODE_RGMII_TXID;
        let rx_delay = iface == bindings::phy_interface_t_PHY_INTERFACE_MODE_RGMII_ID
            || iface == bindings::phy_interface_t_PHY_INTERFACE_MODE_RGMII_RXID;

        dev.modify_paged(
            0xd08,
            TX_DELAY_REG,
            TX_DELAY_EN,
            if tx_delay { TX_DELAY_EN } else { 0 },
        )?;
        dev.modify_paged(
            0xd08,
            RX_DELAY_REG,
            RX_DELAY_EN,
            if rx_delay { RX_DELAY_EN } else { 0 },
        )?;

        // Disable PHY-level EEE so LPI is passed up to the MAC layer.
        // PHYCR2 is on the default page, no page switch needed.
        dev.modify(PHYCR2, PHYCR2_PHY_EEE_ENABLE, 0)?;

        // Note: ALDPS and CLKOUT configuration are omitted because they
        // depend on device tree properties and per-device private data,
        // which are not yet supported by the Rust PHY abstraction.
        // The hardware defaults are preserved for these settings.

        Ok(())
    }

    fn read_page(dev: &mut phy::Device) -> Result<u16> {
        dev.read(PAGE_SELECT)
    }

    fn write_page(dev: &mut phy::Device, page: u16) -> Result {
        dev.write(PAGE_SELECT, page)
    }

    fn config_intr(dev: &mut phy::Device) -> Result {
        if dev.is_interrupts_enabled() {
            // Acknowledge any pending interrupt.
            dev.read(INSR)?;
            // Enable link status change interrupt.
            dev.write_paged(0xa42, INER_REG, INER_LINK_STATUS)?;
        } else {
            // Disable all interrupts.
            dev.write_paged(0xa42, INER_REG, 0)?;
            // Acknowledge any pending interrupt.
            dev.read(INSR)?;
        }
        Ok(())
    }

    fn handle_interrupt(dev: &mut phy::Device) -> IrqReturn {
        let irq_status = match dev.read(INSR) {
            Ok(val) => val,
            Err(_) => {
                dev.phy_error();
                return IrqReturn::None;
            }
        };

        if irq_status & INER_LINK_STATUS != 0 {
            dev.trigger_machine();
            return IrqReturn::Handled;
        }

        // Note: PME/Wake-on-LAN interrupt (bit 7) is not handled because
        // WoL support requires pm_wakeup_event() binding and per-device
        // private data, which are not yet available.

        IrqReturn::None
    }
}
