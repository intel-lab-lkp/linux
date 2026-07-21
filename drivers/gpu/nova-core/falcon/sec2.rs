// SPDX-License-Identifier: GPL-2.0

use kernel::{
    io::{
        Io,
        Mmio,
        Region, //
    },
    sizes::SZ_4K,
};

use crate::{
    driver::Bar0,
    falcon::FalconEngine, //
};

/// Type specifying the `Sec2` falcon engine. Cannot be instantiated.
pub(crate) struct Sec2(());

impl FalconEngine for Sec2 {
    #[inline]
    fn pfalcon(io: Bar0<'_>) -> Mmio<'_, super::PFalconRegisters> {
        Region::subregion::<0x00840000, SZ_4K, _>(io).cast()
    }

    #[inline]
    fn pfalcon2(io: Bar0<'_>) -> Mmio<'_, super::PFalcon2Registers> {
        Region::subregion::<0x00841000, SZ_4K, _>(io).cast()
    }
}
