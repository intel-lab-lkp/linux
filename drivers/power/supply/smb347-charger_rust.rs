// SPDX-License-Identifier: GPL-2.0

//! Rust SMB347 i2c driver

use kernel::{
    device::Core,
    i2c,
    of,
    prelude::*, //
    sync::aref::ARef,
};

/// SMB347 register map. Ported from drivers/power/supply/smb347-charger.c.
#[allow(dead_code)]
mod reg {
    // Configuration registers (0x00–0x0e, write-protected; unlock via CMD_A bit 7)
    pub(crate) const CFG_CHARGE_CURRENT: u8 = 0x00;
    pub(crate) const CFG_CURRENT_LIMIT: u8 = 0x01;
    pub(crate) const CFG_FLOAT_VOLTAGE: u8 = 0x03;
    pub(crate) const CFG_STAT: u8 = 0x05;
    pub(crate) const CFG_PIN: u8 = 0x06;
    pub(crate) const CFG_THERM: u8 = 0x07;
    pub(crate) const CFG_SYSOK: u8 = 0x08;
    pub(crate) const CFG_OTHER: u8 = 0x09;
    pub(crate) const CFG_OTG: u8 = 0x0a;
    pub(crate) const CFG_TEMP_LIMIT: u8 = 0x0b;
    pub(crate) const CFG_FAULT_IRQ: u8 = 0x0c;
    pub(crate) const CFG_STATUS_IRQ: u8 = 0x0d;
    pub(crate) const CFG_ADDRESS: u8 = 0x0e;

    // Command registers
    pub(crate) const CMD_A: u8 = 0x30;
    pub(crate) const CMD_A_ALLOW_WRITE: u8 = 1 << 7;

    pub(crate) const CMD_B: u8 = 0x31;
    pub(crate) const CMD_C: u8 = 0x33;

    // Interrupt-status registers
    pub(crate) const IRQSTAT_A: u8 = 0x35;
    pub(crate) const IRQSTAT_C: u8 = 0x37;
    pub(crate) const IRQSTAT_D: u8 = 0x38;
    pub(crate) const IRQSTAT_E: u8 = 0x39;
    pub(crate) const IRQSTAT_E_DCIN_UV_STAT: u8 = 1 << 4;

    pub(crate) const IRQSTAT_F: u8 = 0x3a;

    // Status registers
    pub(crate) const STAT_A: u8 = 0x3b;
    pub(crate) const STAT_B: u8 = 0x3c;
    pub(crate) const STAT_C: u8 = 0x3d;
    pub(crate) const STAT_C_CHG_SHIFT: u8 = 1;
    pub(crate) const STAT_C_CHG_MASK: u8 = 0x06;

    pub(crate) const STAT_E: u8 = 0x3f;

    pub(crate) const MAX_REGISTER: u8 = 0x3f;
}

// Declared before `client` so it drops first: the supply is
// unregistered (no further get_property callbacks) before the I2C
// client it reads through is released.
struct Smb347 {
    _registration: kernel::power_supply::Registration,
    client: ARef<i2c::I2cClient>,
}

kernel::i2c_device_table! {
    I2C_TABLE,
    MODULE_I2C_TABLE,
    <Smb347 as i2c::Driver>::IdInfo,
    [(i2c::DeviceId::new(c"smb347"), 0)]
}

kernel::of_device_table! {
    OF_TABLE,
    MODULE_OF_TABLE,
    <Smb347 as i2c::Driver>::IdInfo,
    [(of::DeviceId::new(c"summit,smb347"), 0)]
}

impl Smb347 {
    /// Enable/disable writes to the non-volatile config registers (0x00–0x0e)
    /// by toggling CMD_A.ALLOW_WRITE. Other CMD_A bits are preserved.
    #[expect(dead_code)] // TODO: remove once a config-write path calls this (Phase 3)
    fn set_writable(idev: &i2c::I2cClient<Core<'_>>, writable: bool) -> Result {
        let bits = if writable { reg::CMD_A_ALLOW_WRITE } else { 0 };
        idev.smbus_update_bits(reg::CMD_A, reg::CMD_A_ALLOW_WRITE, bits)
    }
}

impl kernel::power_supply::Driver for Smb347 {
    const NAME: &'static CStr = c"smb347-mains";
    const TYPE: kernel::power_supply::Type = kernel::power_supply::TYPE_MAINS;
    const PROPERTIES: &'static [kernel::power_supply::Property] = &[
        kernel::power_supply::PROP_STATUS,
        kernel::power_supply::PROP_ONLINE,
        kernel::power_supply::PROP_CHARGE_TYPE,
    ];

    fn get_property(
        self: Pin<&Self>,
        psp: kernel::power_supply::Property,
        val: &mut kernel::power_supply::PropertyValue,
    ) -> Result {
        if psp == kernel::power_supply::PROP_ONLINE {
            let irqstat_e = self.client.smbus_read_byte_data(reg::IRQSTAT_E)?;
            val.intval = if irqstat_e & reg::IRQSTAT_E_DCIN_UV_STAT == 0 {
                1
            } else {
                0
            };
            Ok(())
        } else if psp == kernel::power_supply::PROP_STATUS {
            let stat_c = self.client.smbus_read_byte_data(reg::STAT_C)?;
            val.intval = if stat_c & reg::STAT_C_CHG_MASK != 0 {
                kernel::power_supply::STATUS_CHARGING
            } else {
                kernel::power_supply::STATUS_NOT_CHARGING
            };
            Ok(())
        } else if psp == kernel::power_supply::PROP_CHARGE_TYPE {
            let stat_c = self.client.smbus_read_byte_data(reg::STAT_C)?;
            let cs = (stat_c & reg::STAT_C_CHG_MASK) >> reg::STAT_C_CHG_SHIFT;
            val.intval = match cs {
                1 => kernel::power_supply::CHARGE_TYPE_TRICKLE,
                2 => kernel::power_supply::CHARGE_TYPE_FAST,
                _ => kernel::power_supply::CHARGE_TYPE_NONE,
            };
            Ok(())
        } else {
            Err(EINVAL)
        }
    }
}

impl i2c::Driver for Smb347 {
    type IdInfo = u32;
    type Data<'bound> = Self;

    const I2C_ID_TABLE: Option<i2c::IdTable<Self::IdInfo>> = Some(&I2C_TABLE);
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        idev: &i2c::I2cClient<Core<'_>>,
        info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let dev = idev.as_ref();

        dev_info!(dev, "Probe Rust SMB347 driver.\n");

        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info);
            let v = idev.smbus_read_byte_data(reg::STAT_A)?;
            dev_info!(dev, "STAT_A = {:#04x}\n", v);
        }
        let client: ARef<i2c::I2cClient> = idev.into();
        let registration = kernel::power_supply::register::<Smb347>(dev)?;
        dev_info!(dev, "registered power_supply\n");
        Ok(Self {
            _registration: registration,
            client,
        })
    }

    fn shutdown(idev: &i2c::I2cClient<Core<'_>>, _this: Pin<&Self>) {
        dev_info!(idev.as_ref(), "Shutdown Rust SMB347 driver.\n");
    }

    fn unbind(idev: &i2c::I2cClient<Core<'_>>, _this: Pin<&Self>) {
        dev_info!(idev.as_ref(), "Unbind Rust SMB347 driver.\n");
    }
}

kernel::module_i2c_driver! {
    type: Smb347,
    name: "smb347_rust",
    authors: ["Bruce Robertson"],
    description: "Rust SMB347 driver",
    license: "GPL v2",
}
