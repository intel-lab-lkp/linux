// SPDX-License-Identifier: GPL-2.0-only

//! Nintendo Wii and Wii U OTP driver
//!
//! This is a driver exposing the OTP of a Nintendo Wii or Wii U console.
//!
//! This memory contains common and per-console keys, signatures and
//! related data required to access peripherals.
//!
//! Based on reversed documentation from https://wiiubrew.org/wiki/Hardware/OTP
//!
//! Copyright (C) 2021 Link Mauve <linkmauve@linkmauve.fr>

use kernel::{
    device::Core,
    devres::Devres,
    io::{mem::ExclusiveIoMem, Io},
    nvmem::{self, NvmemConfig, NvmemProvider},
    of::{DeviceId, IdTable},
    platform,
    prelude::*,
};

const HW_OTPCMD: usize = 0;
const HW_OTPDATA: usize = 4;
const OTP_READ: u32 = 0x80000000;
const BANK_SIZE: u32 = 128;
const WORD_SIZE: u32 = 4;

struct Info {
    name: &'static CStr,
    num_banks: u32,
}

const WII_INFO: Info = Info {
    name: c"wii-otp",
    num_banks: 1,
};

const WIIU_INFO: Info = Info {
    name: c"wiiu-otp",
    num_banks: 8,
};

struct NintendoOtpDriver {
    #[expect(dead_code)]
    iomem: Pin<KBox<Devres<ExclusiveIoMem<8>>>>,
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <NintendoOtpDriver as platform::Driver>::IdInfo,
    [
        (DeviceId::new(c"nintendo,hollywood-otp"), WII_INFO),
        (DeviceId::new(c"nintendo,latte-otp"), WIIU_INFO),
    ]
);

#[derive(Default)]
struct NintendoOtpProvider;

#[vtable]
impl NvmemProvider for NintendoOtpProvider {
    type Priv = Io<8>;

    fn read(io: &Self::Priv, mut reg: u32, mut data: &mut [u8]) -> Result {
        while let Some(bytes) = data.split_off_mut(..4) {
            let bank = (reg / BANK_SIZE) << 8;
            let addr = (reg / WORD_SIZE) % (BANK_SIZE / WORD_SIZE);
            io.write32be(OTP_READ | bank | addr, HW_OTPCMD);
            let elem = io.read32be(HW_OTPDATA);
            bytes.copy_from_slice(&elem.to_be_bytes());
            reg += WORD_SIZE;
        }

        Ok(())
    }

    fn write(_context: &Self::Priv, _offset: u32, _data: &[u8]) -> Result {
        Err(ENODEV)
    }
}

impl platform::Driver for NintendoOtpDriver {
    type IdInfo = Info;
    const OF_ID_TABLE: Option<IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let dev = pdev.as_ref();

        let Some(Info { name, num_banks }) = info else {
            return Err(EINVAL);
        };

        dev_info!(dev, "got '{name}', num_banks = {num_banks}\n");

        let request = pdev.io_request_by_index(0).ok_or(ENODEV)?;
        let iomem = request.iomap_exclusive_sized::<8>();
        let iomem = KBox::pin_init(iomem, GFP_KERNEL)?;
        let io = iomem.access(dev)?;

        let config = NvmemConfig::<NintendoOtpProvider>::default()
            .with_name(name)
            .with_type(nvmem::Type::Otp)
            .with_size((num_banks * BANK_SIZE) as i32)
            .with_word_size(WORD_SIZE as i32)
            .with_stride(WORD_SIZE as i32)
            .with_read_only(true)
            .with_root_only(true);

        dev.nvmem_register(config, io);

        Ok(Self { iomem })
    }
}

kernel::module_platform_driver! {
    type: NintendoOtpDriver,
    name: "nintendo-otp",
    authors: ["Link Mauve <linkmauve@linkmauve.fr>"],
    description: "Nintendo Wii and Wii U OTP driver",
    license: "GPL v2",
}
