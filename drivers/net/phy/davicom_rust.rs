// SPDX-License-Identifier: GPL-2.0-or-later

//! Davicom PHY Rust driver.
//!
//! C version `drivers/net/phy/davicom.c`

use kernel::net::phy::{self, reg::C22, DeviceId, Driver};
use kernel::prelude::*;

/// Register 0x10: Scrambler Control Register (SCR)
const SCR: C22 = C22::vendor_specific::<0x10>();
const SCR_INIT: u16 = 0x0610;
const SCR_RMII: u16 = 0x0100;

/* DM9161 Interrupt Register */

/// Register 0x15: Interrupt Register
const INTR: C22 = C22::vendor_specific::<0x15>();
const INTR_PEND: u16 = 0x8000;
const INTR_DPLX_MASK: u16 = 0x0800;
const INTR_SPD_MASK: u16 = 0x0400;
const INTR_LINK_MASK: u16 = 0x0200;
const INTR_MASK: u16 = 0x0100;
const INTR_DPLX_CHANGE: u16 = 0x0010;
const INTR_SPD_CHANGE: u16 = 0x0008;
const INTR_LINK_CHANGE: u16 = 0x0004;
const INTR_INIT: u16 = 0x0000;
const INTR_STOP: u16 = INTR_DPLX_MASK | INTR_SPD_MASK | INTR_LINK_MASK | INTR_MASK;
const INTR_CHANGE: u16 = INTR_DPLX_CHANGE | INTR_SPD_CHANGE | INTR_LINK_CHANGE;

/// Register 0x12: 10Base-T Configuration/Status Register
const BTCSR: C22 = C22::vendor_specific::<0x12>();
const BTCSR_INIT: u16 = 0x7800;

/// Handles incoming hardware interrupts from the Davicom PHY.
///
/// This checks if the interrupt was caused by a link, speed, or duplex change.
/// If so, it notifies the kernel to update the link state using `genphy_update_link`.
///
/// TODO: This function is currently unused because the Rust PHY abstraction `net::phy::Driver`
/// does not yet expose a `handle_interrupt` callback. It is included here for the RFC.
#[allow(dead_code)]
fn dm9161_handle_interrupt(dev: &mut phy::Device) -> Result {
    let irq_status = dev.read(INTR)?;

    if (irq_status & INTR_CHANGE) == 0 {
        return Ok(());
    }

    dev.genphy_update_link()?;

    Ok(())
}

#[allow(dead_code)]
fn dm9161_ack_interrupt(dev: &mut phy::Device) -> Result {
    let _ = dev.read(INTR)?;
    Ok(())
}

/// Configures whether the hardware alarm (interrupts) should be turned on or off.
///
/// It reads the current interrupt status requested by the OS by accessing the raw pointer.
///
/// TODO: This function is currently unused because the Rust PHY abstraction `net::phy::Driver`
/// does not yet expose a `config_intr` callback. It is included here for the RFC.
#[allow(dead_code)]
fn dm9161_config_intr(dev: &mut phy::Device) -> Result {
    let mut temp = dev.read(INTR)?;

    let intr_enabled = unsafe {
        let ptr = (dev as *mut phy::Device).cast::<bindings::phy_device>();
        (*ptr).interrupts == bindings::PHY_INTERRUPT_ENABLED as u8
    };

    if intr_enabled {
        dm9161_ack_interrupt(dev)?;
        temp &= !INTR_STOP;
        dev.write(INTR, temp)?;
    } else {
        temp |= INTR_STOP;
        dev.write(INTR, temp)?;
        dm9161_ack_interrupt(dev)?;
    }
    Ok(())
}

/// Configures PHY Auto-Negotiation.
///
/// Isolates the PHY during configuration, then calls the generic `genphy_config_aneg`
/// via unsafe C bindings because Rust abstractions don't expose it directly yet.
fn dm9161_config_aneg(dev: &mut phy::Device) -> Result {
    dev.write(C22::BMCR, bindings::BMCR_ISOLATE as u16)?;

    let err = unsafe {
        let ptr = (dev as *mut phy::Device).cast::<bindings::phy_device>();
        bindings::genphy_config_aneg(ptr)
    };
    to_result(err)?;

    Ok(())
}

/// Initializes the Davicom PHY hardware upon detection.
///
/// Depending on the `interface` mode (MII vs RMII), the Scrambler Control Register (SCR)
/// is configured. It relies on `C22::vendor_specific` addresses.
///
/// TODO: This function is currently unused because the Rust PHY abstraction `net::phy::Driver`
/// does not yet expose a `config_init` callback. It is included here for the RFC.
#[allow(dead_code)]
fn dm9161_config_init(dev: &mut phy::Device) -> Result {
    dev.write(C22::BMCR, bindings::BMCR_ISOLATE as u16)?;

    let interface = unsafe {
        let ptr = (dev as *mut phy::Device).cast::<bindings::phy_device>();
        (*ptr).interface
    };

    let temp = match interface as core::ffi::c_uint {
        bindings::phy_interface_t_PHY_INTERFACE_MODE_MII => SCR_INIT,
        bindings::phy_interface_t_PHY_INTERFACE_MODE_RMII => SCR_INIT | SCR_RMII,
        _ => return Err(code::EINVAL),
    };

    dev.write(SCR, temp)?;

    dev.write(BTCSR, BTCSR_INIT)?;

    dev.write(C22::BMCR, bindings::BMCR_ANENABLE as u16)?;

    Ok(())
}

/// Representation of the Davicom DM9161E chip.
struct DavicomDM9161E;

#[vtable]
impl Driver for DavicomDM9161E {
    const NAME: &'static CStr = c"Davicom DM9161E";
    const PHY_DEVICE_ID: phy::DeviceId = DeviceId::new_with_custom_mask(0x0181b880, 0x0ffffff0);
    fn config_aneg(dev: &mut phy::Device) -> Result {
        dm9161_config_aneg(dev)
    }
}

struct DavicomDM9161A;

#[vtable]
impl Driver for DavicomDM9161A {
    const NAME: &'static CStr = c"Davicom DM9161A";
    const PHY_DEVICE_ID: phy::DeviceId = DeviceId::new_with_custom_mask(0x0181b8a0, 0x0ffffff0);
    fn config_aneg(dev: &mut phy::Device) -> Result {
        dm9161_config_aneg(dev)
    }
}

kernel::module_phy_driver! {
    drivers: [DavicomDM9161E, DavicomDM9161A],
    device_table: [DeviceId::new_with_driver::<DavicomDM9161E>(), DeviceId::new_with_driver::<DavicomDM9161A>()],
    name: "davicom_rust",
    authors: ["Andy Fleming"],
    description: "Davicom PHY Rust Driver",
    license: "GPL"
}
