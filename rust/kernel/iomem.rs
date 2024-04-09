// SPDX-License-Identifier: GPL-2.0

use crate::bindings;
use crate::error::{code::EINVAL, Result};

/// IO-mapped memory, starting at the base pointer @ioptr and spanning @malxen bytes.
///
/// The creator (usually a subsystem such as PCI) is responsible for creating the
/// mapping, performing an additional region request etc.
pub struct IoMem {
    pub ioptr: usize,
    maxlen: usize,
}

impl IoMem {
    pub(crate) fn new(ioptr: usize, maxlen: usize) -> Result<Self> {
        if ioptr == 0 || maxlen == 0 {
            return Err(EINVAL);
        }

        Ok(Self { ioptr, maxlen })
    }

    fn get_io_addr(&self, offset: usize, len: usize) -> Result<usize> {
        if offset + len > self.maxlen {
            return Err(EINVAL);
        }

        Ok(self.ioptr + offset)
    }

    pub fn readb(&self, offset: usize) -> Result<u8> {
        let ioptr: usize = self.get_io_addr(offset, 1)?;

        Ok(unsafe { bindings::readb(ioptr as _) })
    }

    pub fn readw(&self, offset: usize) -> Result<u16> {
        let ioptr: usize = self.get_io_addr(offset, 2)?;

        Ok(unsafe { bindings::readw(ioptr as _) })
    }

    pub fn readl(&self, offset: usize) -> Result<u32> {
        let ioptr: usize = self.get_io_addr(offset, 4)?;

        Ok(unsafe { bindings::readl(ioptr as _) })
    }

    pub fn readq(&self, offset: usize) -> Result<u64> {
        let ioptr: usize = self.get_io_addr(offset, 8)?;

        Ok(unsafe { bindings::readq(ioptr as _) })
    }

    pub fn readb_relaxed(&self, offset: usize) -> Result<u8> {
        let ioptr: usize = self.get_io_addr(offset, 1)?;

        Ok(unsafe { bindings::readb_relaxed(ioptr as _) })
    }

    pub fn readw_relaxed(&self, offset: usize) -> Result<u16> {
        let ioptr: usize = self.get_io_addr(offset, 2)?;

        Ok(unsafe { bindings::readw_relaxed(ioptr as _) })
    }

    pub fn readl_relaxed(&self, offset: usize) -> Result<u32> {
        let ioptr: usize = self.get_io_addr(offset, 4)?;

        Ok(unsafe { bindings::readl_relaxed(ioptr as _) })
    }

    pub fn readq_relaxed(&self, offset: usize) -> Result<u64> {
        let ioptr: usize = self.get_io_addr(offset, 8)?;

        Ok(unsafe { bindings::readq_relaxed(ioptr as _) })
    }

    pub fn writeb(&self, byte: u8, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 1)?;

        unsafe { bindings::writeb(byte, ioptr as _) }
        Ok(())
    }

    pub fn writew(&self, word: u16, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 2)?;

        unsafe { bindings::writew(word, ioptr as _) }
        Ok(())
    }

    pub fn writel(&self, lword: u32, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 4)?;

        unsafe { bindings::writel(lword, ioptr as _) }
        Ok(())
    }

    pub fn writeq(&self, qword: u64, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 8)?;

        unsafe { bindings::writeq(qword, ioptr as _) }
        Ok(())
    }

    pub fn writeb_relaxed(&self, byte: u8, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 1)?;

        unsafe { bindings::writeb_relaxed(byte, ioptr as _) }
        Ok(())
    }

    pub fn writew_relaxed(&self, word: u16, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 2)?;

        unsafe { bindings::writew_relaxed(word, ioptr as _) }
        Ok(())
    }

    pub fn writel_relaxed(&self, lword: u32, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 4)?;

        unsafe { bindings::writel_relaxed(lword, ioptr as _) }
        Ok(())
    }

    pub fn writeq_relaxed(&self, qword: u64, offset: usize) -> Result {
        let ioptr: usize = self.get_io_addr(offset, 8)?;

        unsafe { bindings::writeq_relaxed(qword, ioptr as _) }
        Ok(())
    }
}
