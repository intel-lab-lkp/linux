// SPDX-License-Identifier: GPL-2.0

//! FSP (Firmware System Processor) falcon engine for Hopper/Blackwell GPUs.
//!
//! The FSP falcon handles secure boot and Chain of Trust operations
//! on Hopper and Blackwell architectures, replacing SEC2's role.

use kernel::{
    io::{
        Io,
        IoCapable, //
    },
    prelude::*, //
};

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
    const BASE: usize = 0x8f2000;
}

impl RegisterBase<PFalcon2Base> for Fsp {
    const BASE: usize = 0x8f3000;
}

impl FalconEngine for Fsp {
    const ID: Self = Fsp(());
}

/// Maximum addressable EMEM size, derived from the 24-bit offset field
/// in NV_PFALCON_FALCON_EMEM_CTL.
const EMEM_MAX_SIZE: usize = 1 << 24;

/// I/O backend for the FSP falcon's external memory (EMEM).
///
/// Each 32-bit access programs a byte offset via the EMEM_CTL register,
/// then reads or writes through the EMEM_DATA register.
pub(crate) struct Emem<'a> {
    bar: &'a Bar0,
}

impl<'a> Emem<'a> {
    fn new(bar: &'a Bar0) -> Self {
        Self { bar }
    }
}

impl IoCapable<u32> for Emem<'_> {
    unsafe fn io_read(&self, address: usize) -> u32 {
        // The Io trait validates that EMEM accesses fit within the 24-bit offset field.
        let offset = address as u32;

        regs::NV_PFALCON_FALCON_EMEM_CTL::default()
            .set_rd_mode(true)
            .set_offset(offset)
            .write(self.bar, &Fsp::ID);

        regs::NV_PFALCON_FALCON_EMEM_DATA::read(self.bar, &Fsp::ID).data()
    }

    unsafe fn io_write(&self, value: u32, address: usize) {
        // The Io trait validates that EMEM accesses fit within the 24-bit offset field.
        let offset = address as u32;

        regs::NV_PFALCON_FALCON_EMEM_CTL::default()
            .set_wr_mode(true)
            .set_offset(offset)
            .write(self.bar, &Fsp::ID);

        regs::NV_PFALCON_FALCON_EMEM_DATA::default()
            .set_data(value)
            .write(self.bar, &Fsp::ID);
    }
}

impl Io for Emem<'_> {
    fn addr(&self) -> usize {
        0
    }

    fn maxsize(&self) -> usize {
        EMEM_MAX_SIZE
    }
}

impl Falcon<Fsp> {
    /// Returns an EMEM I/O accessor for this FSP falcon.
    pub(crate) fn emem<'a>(&self, bar: &'a Bar0) -> Emem<'a> {
        Emem::new(bar)
    }

    /// Writes `data` to FSP external memory at byte `offset`.
    ///
    /// Data is interpreted as little-endian 32-bit words.
    /// Returns `EINVAL` if offset or data length is not 4-byte aligned.
    #[expect(unused)]
    pub(crate) fn write_emem(&self, bar: &Bar0, offset: u32, data: &[u8]) -> Result {
        if offset % 4 != 0 || data.len() % 4 != 0 {
            return Err(EINVAL);
        }

        let emem = self.emem(bar);
        let mut off = offset as usize;
        for chunk in data.chunks_exact(4) {
            let word = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
            emem.try_write32(word, off)?;
            off += 4;
        }

        Ok(())
    }

    /// Reads FSP external memory at byte `offset` into `data`.
    ///
    /// Data is stored as little-endian 32-bit words.
    /// Returns `EINVAL` if offset or data length is not 4-byte aligned.
    #[expect(unused)]
    pub(crate) fn read_emem(&self, bar: &Bar0, offset: u32, data: &mut [u8]) -> Result {
        if offset % 4 != 0 || data.len() % 4 != 0 {
            return Err(EINVAL);
        }

        let emem = self.emem(bar);
        let mut off = offset as usize;
        for chunk in data.chunks_exact_mut(4) {
            let word = emem.try_read32(off)?;
            chunk.copy_from_slice(&word.to_le_bytes());
            off += 4;
        }

        Ok(())
    }
}
