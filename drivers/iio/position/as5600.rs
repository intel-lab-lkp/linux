// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Muchamad Coirul Anwar <muchamadcoirulanwar@gmail.com>
//! Driver for ams AS5600 12-bit magnetic rotary position sensor.
//!
//! Datasheet: https://look.ams-osram.com/m/7059eac7531a86fd/original/AS5600-DS000365.pdf

use kernel::{
    bindings::{
        iio_chan_info_enum_IIO_CHAN_INFO_RAW,
        iio_chan_info_enum_IIO_CHAN_INFO_SCALE,
        iio_chan_spec,
        iio_chan_type_IIO_ANGL, //
    },
    bits::{
        bit_u8,
        genmask_u16, //
    },
    device::Core,
    error::code::{
        EINVAL,
        ENODATA, //
    },
    i2c::{
        DeviceId,
        Driver,
        I2cClient,
        IdTable, //
    },
    i2c_device_table,
    iio::{
        Device,
        IioDriver,
        IioVal,
        Registered, //
    },
    io::Io,
    module_i2c_driver,
    of,
    of_device_table,
    prelude::*, //
    sync::{
        aref::ARef,
        new_mutex,
        Mutex, //
    },
};

const AS5600_REG_STATUS: u8 = 0x0B;
const AS5600_REG_RAW_ANGLE_H: u8 = 0x0C;

const AS5600_STATUS_MD: u8 = bit_u8(5);
const AS5600_RAW_ANGLE_MASK: u16 = genmask_u16(0..=11);

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

struct As5600Channels([iio_chan_spec; 1]);

// SAFETY: `iio_chan_spec` is a plain C struct (all fields are integers/pointers)
// with no interior mutability. The static is only read after initialization,
// making shared access safe.
unsafe impl Sync for As5600Channels {}

static AS5600_CHANNELS: As5600Channels = As5600Channels({
    // SAFETY: `iio_chan_spec` is a repr(C) struct where all-zeroes is valid
    // (integers default to 0, pointers to NULL).
    let mut chan: iio_chan_spec = unsafe { core::mem::zeroed() };
    chan.type_ = iio_chan_type_IIO_ANGL;
    // TODO: Use kernel::bits equivalent once bit_usize exists
    chan.info_mask_separate = (1usize << iio_chan_info_enum_IIO_CHAN_INFO_RAW)
        | (1usize << iio_chan_info_enum_IIO_CHAN_INFO_SCALE);
    [chan]
});

#[pin_data]
struct As5600Priv {
    #[pin]
    io_lock: Mutex<As5600HwState>,
}

struct As5600HwState {
    client: ARef<I2cClient>,
}

impl IioDriver for As5600Priv {
    fn read_raw(&self, _chan: *const iio_chan_spec, mask: isize) -> Result<IioVal> {
        const INFO_RAW: isize = iio_chan_info_enum_IIO_CHAN_INFO_RAW as isize;
        const INFO_SCALE: isize = iio_chan_info_enum_IIO_CHAN_INFO_SCALE as isize;
        match mask {
            // IIO_CHAN_INFO_RAW: read the 12-bit raw angle value.
            INFO_RAW => {
                let hw = self.io_lock.lock();

                // Read status register to verify magnet presence before
                // reading the angle.
                let status = hw.client.try_read8(AS5600_REG_STATUS as usize)?;

                // Check magnet presence (MD bit). Without a magnet the angle
                // register contains stale/invalid data.
                if (status & AS5600_STATUS_MD) == 0 {
                    return Err(ENODATA);
                }

                // Word read at register 0x0C: SMBus read_word_data returns LE,
                // AS5600 stores angle big-endian, so swap_bytes() is needed.
                // Mutex ensures status + angle read is atomic.
                // NOTE: Equivalent to C's i2c_smbus_read_word_swapped().
                // Long-term, regmap-rs with val_format_endian=Big handles
                // this transparently at configuration level.
                let raw = hw.client.try_read16(AS5600_REG_RAW_ANGLE_H as usize)?;
                let angle = raw.swap_bytes() & AS5600_RAW_ANGLE_MASK;
                Ok(IioVal::Int(angle as i32))
            }
            // IIO_CHAN_INFO_SCALE: radians per LSB, 2*pi / 4096 = 0.001533981.
            INFO_SCALE => {
                Ok(IioVal::IntPlusNano(0, 1533981))
            }
            _ => Err(EINVAL),
        }
    }

    fn channels(&self) -> &[iio_chan_spec] {
        &AS5600_CHANNELS.0
    }
}

#[pin_data]
struct As5600 {
    #[pin]
    _iio_dev: Device<As5600Priv, Registered>,
}

impl Driver for As5600 {
    type IdInfo = ();
    type Data<'bound> = As5600;

    const I2C_ID_TABLE: Option<IdTable<Self::IdInfo>> = Some(&I2C_TABLE);
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    #[allow(refining_impl_trait)]
    fn probe<'bound>(
        dev: &'bound I2cClient<Core<'_>>,
        _id_info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        try_pin_init!(As5600 {
            _iio_dev: {
                let client: ARef<I2cClient> = dev.into();

                let priv_init = pin_init!(As5600Priv {
                    io_lock <- new_mutex!(As5600HwState {
                       client
                    }),
                });

                let iio_dev = Device::build_device(dev.as_ref(), c"as5600", priv_init)?;
                let registered = iio_dev.register(&crate::THIS_MODULE)?;
                dev_dbg!(dev.as_ref(), "AS5600 magnetic position sensor ready\n");
                registered
            }
        })
    }
}
