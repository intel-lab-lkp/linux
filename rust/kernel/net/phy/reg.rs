// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 FUJITA Tomonori <fujita.tomonori@gmail.com>

//! PHY registers.
//!
//! This module provides an interface for PHY registers defined in IEEE 802.3
//! Clause 22 and 45.

use super::Device;
use crate::build_assert;
use crate::error::*;
use crate::uapi;

/// Accesses PHY registers.
///
/// This trait is used to implement the unified interface to access
/// C22 and C45 PHY registers.
pub trait Register {
    /// Reads a PHY register.
    fn read(&self, dev: &mut Device) -> Result<u16>;

    /// Writes a PHY register.
    fn write(&self, dev: &mut Device, val: u16) -> Result;

    /// Checks the link status and updates current link state.
    fn read_status(dev: &mut Device) -> Result<u16>;
}

/// C22 registers.
pub struct C22(u8);

impl C22 {
    /// Basic mode control.
    pub const BMCR: Self = C22(0x00);
    /// Basic mode status.
    pub const BMSR: Self = C22(0x01);
    /// PHY identifier 1.
    pub const PHYSID1: Self = C22(0x02);
    /// PHY identifier 2.
    pub const PHYSID2: Self = C22(0x03);
    /// Auto-negotiation advertisement.
    pub const ADVERTISE: Self = C22(0x04);
    /// Auto-negotiation link partner base page ability.
    pub const LPA: Self = C22(0x05);
    /// Auto-negotiation expansion.
    pub const EXPANSION: Self = C22(0x06);
    /// Auto-negotiation next page transmit.
    pub const NEXT_PAGE_TRANSMIT: Self = C22(0x07);
    /// Auto-negotiation link partner received next page.
    pub const LP_RECEIVED_NEXT_PAGE: Self = C22(0x08);
    /// Master-slave control.
    pub const MASTER_SLAVE_CONTROL: Self = C22(0x09);
    /// Master-slave status.
    pub const MASTER_SLAVE_STATUS: Self = C22(0x0a);
    /// PSE Control.
    pub const PSE_CONTROL: Self = C22(0x0b);
    /// PSE Status.
    pub const PSE_STATUS: Self = C22(0x0c);
    /// MMD Register control.
    pub const MMD_CONTROL: Self = C22(0x0d);
    /// MMD Register address data.
    pub const MMD_DATA: Self = C22(0x0e);
    /// Extended status.
    pub const EXTENDED_STATUS: Self = C22(0x0f);

    /// Creates a new instance of `C22` with a vendor specific register.
    pub const fn vendor_specific<const N: u8>() -> Self {
        build_assert!(
            N > 0x0f && N < 0x20,
            "Vendor-specific register address must be between 16 and 31"
        );
        C22(N)
    }
}

impl Register for C22 {
    fn read(&self, dev: &mut Device) -> Result<u16> {
        let phydev = dev.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Device`.
        // So it's just an FFI call, open code of `phy_read()` with a valid `phy_device` pointer
        // `phydev`.
        let ret = unsafe {
            bindings::mdiobus_read((*phydev).mdio.bus, (*phydev).mdio.addr, self.0.into())
        };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    fn write(&self, dev: &mut Device, val: u16) -> Result {
        let phydev = dev.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Device`.
        // So it's just an FFI call, open code of `phy_write()` with a valid `phy_device` pointer
        // `phydev`.
        to_result(unsafe {
            bindings::mdiobus_write((*phydev).mdio.bus, (*phydev).mdio.addr, self.0.into(), val)
        })
    }

    fn read_status(dev: &mut Device) -> Result<u16> {
        let phydev = dev.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So it's just an FFI call.
        let ret = unsafe { bindings::genphy_read_status(phydev) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }
}

/// MDIO manageable devices.
pub struct Mmd(u8);

impl Mmd {
    /// Physical Medium Attachment/Dependent.
    pub const PMAPMD: Self = Mmd(uapi::MDIO_MMD_PMAPMD as u8);
    /// WAN interface sublayer.
    pub const WIS: Self = Mmd(uapi::MDIO_MMD_WIS as u8);
    /// Physical coding sublayer.
    pub const PCS: Self = Mmd(uapi::MDIO_MMD_PCS as u8);
    /// PHY Extender sublayer.
    pub const PHYXS: Self = Mmd(uapi::MDIO_MMD_PHYXS as u8);
    /// DTE Extender sublayer.
    pub const DTEXS: Self = Mmd(uapi::MDIO_MMD_DTEXS as u8);
    /// Transmission convergence.
    pub const TC: Self = Mmd(uapi::MDIO_MMD_TC as u8);
    /// Auto negotiation.
    pub const AN: Self = Mmd(uapi::MDIO_MMD_AN as u8);
    /// Separated PMA (1).
    pub const SEPARATED_PMA1: Self = Mmd(8);
    /// Separated PMA (2).
    pub const SEPARATED_PMA2: Self = Mmd(9);
    /// Separated PMA (3).
    pub const SEPARATED_PMA3: Self = Mmd(10);
    /// Separated PMA (4).
    pub const SEPARATED_PMA4: Self = Mmd(11);
    /// OFDM PMA/PMD.
    pub const OFDM_PMAPMD: Self = Mmd(12);
    /// Power unit.
    pub const POWER_UNIT: Self = Mmd(13);
    /// Clause 22 extension.
    pub const C22_EXT: Self = Mmd(uapi::MDIO_MMD_C22EXT as u8);
    /// Vendor specific 1.
    pub const VEND1: Self = Mmd(uapi::MDIO_MMD_VEND1 as u8);
    /// Vendor specific 2.
    pub const VEND2: Self = Mmd(uapi::MDIO_MMD_VEND2 as u8);
}

/// C45 registers.
pub struct C45 {
    devad: Mmd,
    regnum: u16,
}

impl C45 {
    /// Creates a new instance of `C45`.
    pub fn new(devad: Mmd, regnum: u16) -> Self {
        Self { devad, regnum }
    }
}

impl Register for C45 {
    fn read(&self, dev: &mut Device) -> Result<u16> {
        let phydev = dev.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Device`.
        // So it's just an FFI call.
        let ret =
            unsafe { bindings::phy_read_mmd(phydev, self.devad.0.into(), self.regnum.into()) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }

    fn write(&self, dev: &mut Device, val: u16) -> Result {
        let phydev = dev.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Device`.
        // So it's just an FFI call.
        to_result(unsafe {
            bindings::phy_write_mmd(phydev, self.devad.0.into(), self.regnum.into(), val)
        })
    }

    fn read_status(dev: &mut Device) -> Result<u16> {
        let phydev = dev.0.get();
        // SAFETY: `phydev` is pointing to a valid object by the type invariant of `Self`.
        // So it's just an FFI call.
        let ret = unsafe { bindings::genphy_c45_read_status(phydev) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as u16)
        }
    }
}
