// SPDX-License-Identifier: GPL-2.0
// Copyright (C) Tehuti Networks Ltd.
// Copyright (C) 2024 FUJITA Tomonori <fujita.tomonori@gmail.com>

//! Applied Micro Circuits Corporation QT2025 PHY driver
use kernel::c_str;
use kernel::net::phy::{self, DeviceId, Driver, Firmware};
use kernel::prelude::*;
use kernel::uapi;

kernel::module_phy_driver! {
    drivers: [PhyQT2025],
    device_table: [
        DeviceId::new_with_driver::<PhyQT2025>(),
    ],
    name: "qt2025_phy",
    author: "FUJITA Tomonori <fujita.tomonori@gmail.com>",
    description: "AMCC QT2025 PHY driver",
    license: "GPL",
}

const MDIO_MMD_PMAPMD: u8 = uapi::MDIO_MMD_PMAPMD as u8;
const MDIO_MMD_PCS: u8 = uapi::MDIO_MMD_PCS as u8;
const MDIO_MMD_PHYXS: u8 = uapi::MDIO_MMD_PHYXS as u8;

struct PhyQT2025;

#[vtable]
impl Driver for PhyQT2025 {
    const NAME: &'static CStr = c_str!("QT2025 10Gpbs SFP+");
    const PHY_DEVICE_ID: phy::DeviceId = phy::DeviceId::new_with_exact_mask(0x0043A400);

    fn config_init(dev: &mut phy::Device) -> Result<()> {
        let fw = Firmware::new(c_str!("qt2025-2.0.3.3.fw"), dev)?;

        let phy_id = dev.c45_read(MDIO_MMD_PMAPMD, 0xd001)?;
        if (phy_id >> 8) & 0xff != 0xb3 {
            return Ok(());
        }

        dev.c45_write(MDIO_MMD_PMAPMD, 0xC300, 0x0000)?;
        dev.c45_write(MDIO_MMD_PMAPMD, 0xC302, 0x4)?;
        dev.c45_write(MDIO_MMD_PMAPMD, 0xC319, 0x0038)?;

        dev.c45_write(MDIO_MMD_PMAPMD, 0xC31A, 0x0098)?;
        dev.c45_write(MDIO_MMD_PCS, 0x0026, 0x0E00)?;

        dev.c45_write(MDIO_MMD_PCS, 0x0027, 0x0893)?;

        dev.c45_write(MDIO_MMD_PCS, 0x0028, 0xA528)?;
        dev.c45_write(MDIO_MMD_PCS, 0x0029, 0x03)?;
        dev.c45_write(MDIO_MMD_PMAPMD, 0xC30A, 0x06E1)?;
        dev.c45_write(MDIO_MMD_PMAPMD, 0xC300, 0x0002)?;
        dev.c45_write(MDIO_MMD_PCS, 0xE854, 0x00C0)?;

        let mut j = 0x8000;
        let mut a = MDIO_MMD_PCS;
        for (i, val) in fw.data().iter().enumerate() {
            if i == 0x4000 {
                a = MDIO_MMD_PHYXS;
                j = 0x8000;
            }
            dev.c45_write(a, j, (*val).into())?;

            j += 1;
        }
        dev.c45_write(MDIO_MMD_PCS, 0xe854, 0x0040)?;

        Ok(())
    }

    fn read_status(dev: &mut phy::Device) -> Result<u16> {
        dev.genphy_c45_read_status()
    }
}
