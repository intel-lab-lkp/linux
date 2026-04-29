// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Muchamad Coirul Anwar <muchamadcoirulanwar@gmail.com>
//! Driver for ams AS5600 12-bit magnetic rotary position sensor.
//!
//! Datasheet: https://ams.com/documents/20143/36005/AS5600_DS000365_5-00.pdf

use kernel::{
    bindings::{
        iio_chan_info_enum_IIO_CHAN_INFO_RAW, iio_chan_info_enum_IIO_CHAN_INFO_SCALE,
        iio_chan_spec, iio_chan_type_IIO_ANGL, ENODATA,
    },
    bits::bit_u8,
    device::Core,
    i2c::{DeviceId, Driver, I2cClient, IdTable},
    i2c_device_table,
    iio::{Device, IioDriver, IioVal},
    module_i2c_driver, of, of_device_table,
    prelude::*,
};

const AS5600_REG_STATUS: u8 = 0x0B;
const AS5600_REG_RAW_ANGLE_H: u8 = 0x0C;
const AS5600_REG_RAW_ANGLE_L: u8 = 0x0D;

const AS5600_STATUS_MD: u8 = bit_u8(5);

module_i2c_driver! {
    type: As5600,
    name: "as5600",
    authors: ["Muchamad Coirul Anwar"],
    description: "I2C Driver for ams OSRAM AS5600 Magnetic Rotary Position Sensor",
    license: "GPL",
}

i2c_device_table!(
    I2C_TABLE,
    MODULE_I2C_TABLE,
    <As5600 as Driver>::IdInfo,
    [(DeviceId::new(c"as5600"), ())]
);

of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <As5600 as Driver>::IdInfo,
    [(of::DeviceId::new(c"ams,as5600"), ())]
);

struct As5600Priv {
    client_ptr: *const I2cClient<Core>,
    channels: [iio_chan_spec; 1],
}

// SAFETY: `client_ptr` points to an `I2cClient` that is owned by the I2C
// subsystem and outlives the driver binding. `iio_device_unregister` in
// `Device<T>::PinnedDrop` drains pending callbacks before this struct is
// dropped, so `client_ptr` is valid for every `read_raw` invocation.
// Concurrent access is safe because the I2C adapter lock serializes all
// SMBus transactions.
unsafe impl Send for As5600Priv {}
unsafe impl Sync for As5600Priv {}

impl IioDriver for As5600Priv {
    fn read_raw(&self, _chan: *const iio_chan_spec, mask: isize) -> Result<IioVal> {
        // SAFETY: `client_ptr` was set from a valid `&I2cClient` in `probe()`.
        // The I2C client outlives the driver binding, and `read_raw` is only
        // called while the driver is bound.
        let client = unsafe { &*self.client_ptr };

        #[allow(non_upper_case_globals)]
        match mask as u32 {
            // IIO_CHAN_INFO_RAW
            iio_chan_info_enum_IIO_CHAN_INFO_RAW => {
                let status = client.smbus_read_byte_data(AS5600_REG_STATUS)?;
                if (status & AS5600_STATUS_MD) == 0 {
                    return Err(Error::from_errno(-(ENODATA as i32)));
                }

                let angle_h = client.smbus_read_byte_data(AS5600_REG_RAW_ANGLE_H)? as u16;
                let angle_l = client.smbus_read_byte_data(AS5600_REG_RAW_ANGLE_L)? as u16;

                let angle = (angle_h << 8 | angle_l) & 0x0FFF;
                Ok(IioVal::Int(angle as i32))
            }
            // IIO_CHAN_INFO_SCALE
            iio_chan_info_enum_IIO_CHAN_INFO_SCALE => Ok(IioVal::IntPlusNano(0, 1533981)),
            _ => Err(EINVAL),
        }
    }

    fn channels(&self) -> &[iio_chan_spec] {
        &self.channels
    }
}

struct As5600 {
    _iio_dev: Device<As5600Priv>,
}

impl Driver for As5600 {
    type IdInfo = ();
    const I2C_ID_TABLE: Option<IdTable<Self::IdInfo>> = Some(&I2C_TABLE);
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(dev: &I2cClient<Core>, _id_info: Option<&Self::IdInfo>) -> impl PinInit<Self, Error> {
        let _status = dev.smbus_read_byte_data(AS5600_REG_STATUS)?;

        // SAFETY: `iio_chan_spec` is a C struct whose fields are all integers
        // and pointers. Zero is a valid initialization for all of them.
        let mut channels: [iio_chan_spec; 1] = unsafe { core::mem::zeroed() };
        channels[0].info_mask_separate = (1 << iio_chan_info_enum_IIO_CHAN_INFO_RAW)
            | (1 << iio_chan_info_enum_IIO_CHAN_INFO_SCALE);
        channels[0].type_ = iio_chan_type_IIO_ANGL;

        let priv_data = As5600Priv {
            client_ptr: dev as *const _,
            channels,
        };

        let mut iio_dev = Device::new(dev.as_ref(), priv_data, c"as5600")?;

        iio_dev.register(dev.as_ref(), &crate::THIS_MODULE)?;

        dev_dbg!(dev.as_ref(), "AS5600: Sensor probed, driver ready\n");
        Ok::<_, Error>(As5600 { _iio_dev: iio_dev })
    }

    fn unbind(_dev: &I2cClient<Core>, _this: Pin<&Self>) {}
}
