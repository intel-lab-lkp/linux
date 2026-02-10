// SPDX-License-Identifier: GPL-2.0

//! FSP (Firmware System Processor) falcon engine for Hopper/Blackwell GPUs.
//!
//! The FSP falcon handles secure boot and Chain of Trust operations
//! on Hopper and Blackwell architectures, replacing SEC2's role.

use kernel::prelude::*;

use crate::{
    driver::Bar0,
    falcon::{
        Falcon,
        FalconEngine,
        PFalcon2Base,
        PFalconBase, //
    },
    regs::{
        self,
        macros::RegisterBase, //
    },
};

/// Type specifying the `Fsp` falcon engine. Cannot be instantiated.
pub(crate) struct Fsp(());

impl RegisterBase<PFalconBase> for Fsp {
    // FSP falcon base address for Blackwell
    const BASE: usize = 0x8f2000;
}

impl RegisterBase<PFalcon2Base> for Fsp {
    const BASE: usize = 0x8f3000;
}

impl FalconEngine for Fsp {
    const ID: Self = Fsp(());
}

impl Falcon<Fsp> {
    /// Writes `data` to FSP external memory at byte `offset` using Falcon PIO.
    ///
    /// Returns `EINVAL` if offset or data length is not 4-byte aligned.
    #[expect(unused)]
    pub(crate) fn write_emem(&self, bar: &Bar0, offset: u32, data: &[u8]) -> Result {
        // TODO: replace with `is_multiple_of` once the MSRV is >= 1.82.
        if offset % 4 != 0 || data.len() % 4 != 0 {
            return Err(EINVAL);
        }

        regs::NV_PFALCON_FALCON_EMEM_CTL::default()
            .set_wr_mode(true)
            .set_offset(offset)
            .write(bar, &Fsp::ID);

        for chunk in data.chunks_exact(4) {
            let word = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
            regs::NV_PFALCON_FALCON_EMEM_DATA::default()
                .set_data(word)
                .write(bar, &Fsp::ID);
        }

        Ok(())
    }

    /// Reads FSP external memory at byte `offset` into `data` using Falcon PIO.
    ///
    /// Returns `EINVAL` if offset or data length is not 4-byte aligned.
    #[expect(unused)]
    pub(crate) fn read_emem(&self, bar: &Bar0, offset: u32, data: &mut [u8]) -> Result {
        // TODO: replace with `is_multiple_of` once the MSRV is >= 1.82.
        if offset % 4 != 0 || data.len() % 4 != 0 {
            return Err(EINVAL);
        }

        regs::NV_PFALCON_FALCON_EMEM_CTL::default()
            .set_rd_mode(true)
            .set_offset(offset)
            .write(bar, &Fsp::ID);

        for chunk in data.chunks_exact_mut(4) {
            let word = regs::NV_PFALCON_FALCON_EMEM_DATA::read(bar, &Fsp::ID).data();
            chunk.copy_from_slice(&word.to_le_bytes());
        }

        Ok(())
    }
}
