// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Muchamad Coirul Anwar <muchamadcoirulanwar@gmail.com>
//! Driver for ams AS5600 12-bit magnetic rotary position sensor.
//!
//! Datasheet: https://ams.com/documents/20143/36005/AS5600_DS000365_5-00.pdf

use kernel::device::Core;
use kernel::i2c;
use kernel::module_i2c_driver;
use kernel::prelude::*;

// AS5600 register addresses (from datasheet v1.06)
const AS5600_REG_STATUS: u8 = 0x0B;
const AS5600_REG_RAW_ANGLE_H: u8 = 0x0C; // High nibble [11:8] in bits [3:0]
const AS5600_REG_RAW_ANGLE_L: u8 = 0x0D; // Low byte [7:0]

// STATUS register bit masks
const AS5600_STATUS_MH: u8 = 0x08; // Magnet too strong (AGC minimum gain overflow)
const AS5600_STATUS_ML: u8 = 0x10; // Magnet too weak (AGC maximum gain overflow)
const AS5600_STATUS_MD: u8 = 0x20; // Magnet detected

module_i2c_driver! {
    type: As5600,
    name: "as5600",
    authors: ["Muchamad Coirul Anwar"],
    description: "I2C Driver for ams OSRAM AS5600 Magnetic Rotary Position Sensor",
    license: "GPL",
}

kernel::i2c_device_table!(
    I2C_TABLE,
    MODULE_I2C_TABLE,
    <As5600 as i2c::Driver>::IdInfo,
    [(i2c::DeviceId::new(c"as5600"), ())]
);

struct As5600 {}

impl i2c::Driver for As5600 {
    type IdInfo = ();
    const I2C_ID_TABLE: Option<i2c::IdTable<Self::IdInfo>> = Some(&I2C_TABLE);

    fn probe(
        dev: &i2c::I2cClient<Core>,
        _id_info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let status = dev.smbus_read_byte_data(AS5600_REG_STATUS)?;
        if (status & AS5600_STATUS_MD) == 0 {
            dev_err!(dev.as_ref(), "AS5600: No magnet detected\n");
            return Err(ENODEV);
        } else if (status & AS5600_STATUS_MH) != 0 {
            dev_warn!(dev.as_ref(), "AS5600: Magnet too strong\n");
        } else if (status & AS5600_STATUS_ML) != 0 {
            dev_warn!(dev.as_ref(), "AS5600: Magnet too weak\n");
        }

        dev_info!(dev.as_ref(), "AS5600: Sensor probed, driver ready\n");
        Ok::<_, Error>(As5600 {})
    }

    fn unbind(dev: &i2c::I2cClient<Core>, _this: Pin<&Self>) {
        dev_info!(dev.as_ref(), "AS5600 Sensor Driver Unbound\n");
    }
}
