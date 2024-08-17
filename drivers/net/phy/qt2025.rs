// SPDX-License-Identifier: GPL-2.0
// Copyright (C) Tehuti Networks Ltd.
// Copyright (C) 2024 FUJITA Tomonori <fujita.tomonori@gmail.com>

//! Applied Micro Circuits Corporation QT2025 PHY driver
use kernel::c_str;
use kernel::error::code;
use kernel::firmware::Firmware;
use kernel::net::phy::{
    self,
    reg::{Mmd, C45},
    DeviceId, Driver,
};
use kernel::prelude::*;
use kernel::sizes::{SZ_16K, SZ_32K, SZ_8K};

kernel::module_phy_driver! {
    drivers: [PhyQT2025],
    device_table: [
        DeviceId::new_with_driver::<PhyQT2025>(),
    ],
    name: "qt2025_phy",
    author: "FUJITA Tomonori <fujita.tomonori@gmail.com>",
    description: "AMCC QT2025 PHY driver",
    license: "GPL",
    firmware: ["qt2025-2.0.3.3.fw"],
}

struct PhyQT2025;

#[vtable]
impl Driver for PhyQT2025 {
    const NAME: &'static CStr = c_str!("QT2025 10Gpbs SFP+");
    const PHY_DEVICE_ID: phy::DeviceId = phy::DeviceId::new_with_exact_mask(0x0043A400);

    fn probe(dev: &mut phy::Device) -> Result<()> {
        // The vendor driver does the following checking but we have no idea why.
        let hw_id = dev.read(C45::new(Mmd::PMAPMD, 0xd001))?;
        if (hw_id >> 8) & 0xff != 0xb3 {
            return Err(code::ENODEV);
        }

        // The 8051 will remain in the reset state.
        dev.write(C45::new(Mmd::PMAPMD, 0xC300), 0x0000)?;
        // Configure the 8051 clock frequency.
        dev.write(C45::new(Mmd::PMAPMD, 0xC302), 0x0004)?;
        // Non loopback mode.
        dev.write(C45::new(Mmd::PMAPMD, 0xC319), 0x0038)?;
        // Global control bit to select between LAN and WAN (WIS) mode.
        dev.write(C45::new(Mmd::PMAPMD, 0xC31A), 0x0098)?;
        dev.write(C45::new(Mmd::PCS, 0x0026), 0x0E00)?;
        dev.write(C45::new(Mmd::PCS, 0x0027), 0x0893)?;
        dev.write(C45::new(Mmd::PCS, 0x0028), 0xA528)?;
        dev.write(C45::new(Mmd::PCS, 0x0029), 0x0003)?;
        // Configure transmit and recovered clock.
        dev.write(C45::new(Mmd::PMAPMD, 0xC30A), 0x06E1)?;
        // The 8051 will finish the reset state.
        dev.write(C45::new(Mmd::PMAPMD, 0xC300), 0x0002)?;
        // The 8051 will start running from the boot ROM.
        dev.write(C45::new(Mmd::PCS, 0xE854), 0x00C0)?;

        let fw = Firmware::request(c_str!("qt2025-2.0.3.3.fw"), dev.as_ref())?;
        if fw.data().len() > SZ_16K + SZ_8K {
            return Err(code::EFBIG);
        }

        // The 24kB of program memory space is accessible by MDIO.
        // The first 16kB of memory is located in the address range 3.8000h - 3.BFFFh.
        // The next 8kB of memory is located at 4.8000h - 4.9FFFh.
        let mut j = SZ_32K;
        for (i, val) in fw.data().iter().enumerate() {
            if i == SZ_16K {
                j = SZ_32K;
            }

            let mmd = if i < SZ_16K { Mmd::PCS } else { Mmd::PHYXS };
            dev.write(C45::new(mmd, j as u16), (*val).into())?;

            j += 1;
        }
        // The 8051 will start running from SRAM.
        dev.write(C45::new(Mmd::PCS, 0xE854), 0x0040)?;

        Ok(())
    }

    fn read_status(dev: &mut phy::Device) -> Result<u16> {
        dev.genphy_read_status::<C45>()
    }
}
