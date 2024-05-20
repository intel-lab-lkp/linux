// SPDX-License-Identifier: GPL-2.0

use kernel::{
    device, devres::Devres, error::code::*, firmware, fmt, pci, prelude::*, str::CString, sync::Arc,
};

use core::fmt::Debug;

/// Enum representing the GPU chipset.
#[derive(Debug)]
pub(crate) enum Chipset {
    TU102 = 0x162,
    TU104 = 0x164,
    TU106 = 0x166,
    TU117 = 0x167,
    TU116 = 0x168,
    GA102 = 0x172,
    GA103 = 0x173,
    GA104 = 0x174,
    GA106 = 0x176,
    GA107 = 0x177,
    AD102 = 0x192,
    AD103 = 0x193,
    AD104 = 0x194,
    AD106 = 0x196,
    AD107 = 0x197,
}

/// Enum representing the GPU generation.
#[derive(Debug)]
pub(crate) enum CardType {
    /// Turing
    TU100 = 0x160,
    /// Ampere
    GA100 = 0x170,
    /// Ada Lovelace
    AD100 = 0x190,
}

/// Structure holding the metadata of the GPU.
#[allow(dead_code)]
pub(crate) struct GpuSpec {
    /// Contents of the boot0 register.
    boot0: u64,
    card_type: CardType,
    chipset: Chipset,
    /// The revision of the chipset.
    chiprev: u8,
}

/// Structure encapsulating the firmware blobs required for the GPU to operate.
#[allow(dead_code)]
pub(crate) struct Firmware {
    booter_load: firmware::Firmware,
    booter_unload: firmware::Firmware,
    gsp: firmware::Firmware,
}

/// Structure holding the resources required to operate the GPU.
#[allow(dead_code)]
#[pin_data]
pub(crate) struct Gpu {
    spec: GpuSpec,
    /// MMIO mapping of PCI BAR 0
    bar: Devres<pci::Bar>,
    fw: Firmware,
}

// TODO replace with something like derive(FromPrimitive)
impl Chipset {
    fn from_u32(value: u32) -> Option<Chipset> {
        match value {
            0x162 => Some(Chipset::TU102),
            0x164 => Some(Chipset::TU104),
            0x166 => Some(Chipset::TU106),
            0x167 => Some(Chipset::TU117),
            0x168 => Some(Chipset::TU116),
            0x172 => Some(Chipset::GA102),
            0x173 => Some(Chipset::GA103),
            0x174 => Some(Chipset::GA104),
            0x176 => Some(Chipset::GA106),
            0x177 => Some(Chipset::GA107),
            0x192 => Some(Chipset::AD102),
            0x193 => Some(Chipset::AD103),
            0x194 => Some(Chipset::AD104),
            0x196 => Some(Chipset::AD106),
            0x197 => Some(Chipset::AD107),
            _ => None,
        }
    }
}

// TODO replace with something like derive(FromPrimitive)
impl CardType {
    fn from_u32(value: u32) -> Option<CardType> {
        match value {
            0x160 => Some(CardType::TU100),
            0x170 => Some(CardType::GA100),
            0x190 => Some(CardType::AD100),
            _ => None,
        }
    }
}

impl GpuSpec {
    fn new(bar: &Devres<pci::Bar>) -> Result<GpuSpec> {
        let bar = bar.try_access().ok_or(ENXIO)?;
        let boot0 = u64::from_le(bar.readq(0)?);
        let chip = ((boot0 & 0x1ff00000) >> 20) as u32;

        if boot0 & 0x1f000000 == 0 {
            return Err(ENODEV);
        }

        let chipset = match Chipset::from_u32(chip) {
            Some(x) => x,
            None => return Err(ENODEV),
        };

        let card_type = match CardType::from_u32(chip & 0x1f0) {
            Some(x) => x,
            None => return Err(ENODEV),
        };

        Ok(Self {
            boot0,
            card_type,
            chipset,
            chiprev: (boot0 & 0xff) as u8,
        })
    }
}

impl Firmware {
    fn new(dev: &device::Device, spec: &GpuSpec, ver: &str) -> Result<Firmware> {
        let mut chip_name = CString::try_from_fmt(fmt!("{:?}", spec.chipset))?;
        chip_name.make_ascii_lowercase();

        let fw_booter_load_path =
            CString::try_from_fmt(fmt!("nvidia/{}/gsp/booter_load-{}.bin", &*chip_name, ver,))?;
        let fw_booter_unload_path =
            CString::try_from_fmt(fmt!("nvidia/{}/gsp/booter_unload-{}.bin", &*chip_name, ver))?;
        let fw_gsp_path =
            CString::try_from_fmt(fmt!("nvidia/{}/gsp/gsp-{}.bin", &*chip_name, ver))?;

        let booter_load = firmware::Firmware::request(&fw_booter_load_path, dev)?;
        let booter_unload = firmware::Firmware::request(&fw_booter_unload_path, dev)?;
        let gsp = firmware::Firmware::request(&fw_gsp_path, dev)?;

        Ok(Firmware {
            booter_load,
            booter_unload,
            gsp,
        })
    }
}

impl Gpu {
    pub(crate) fn new(pdev: &pci::Device, bar: Devres<pci::Bar>) -> Result<Arc<Gpu>> {
        let spec = GpuSpec::new(&bar)?;
        let fw = Firmware::new(pdev.as_ref(), &spec, "535.113.01")?;

        dev_info!(
            pdev.as_ref(),
            "NVIDIA {:?} ({:#x})",
            spec.chipset,
            spec.boot0
        );

        Arc::pin_init(try_pin_init!(Self { spec, bar, fw }), GFP_KERNEL)
    }
}
