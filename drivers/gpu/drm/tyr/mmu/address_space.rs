// SPDX-License-Identifier: GPL-2.0 or MIT

//! GPU address space management and hardware operations.
//!
//! This is just a lifetime/resource stub.
//! The actual GPU hardware address spaces (AS), including configuration,
//! command submission, and page table update regions will be added later.

use kernel::{
    io::{
        poll,
        register::Array,
        Io, //
    },
    prelude::*,
    sync::{
        Arc, //
        ArcBorrow,
    },
    time::Delta, //
};

use crate::{
    driver::IoMem,
    regs::mmu_control::mmu_as_control::STATUS, //
};

/// Manages GPU hardware address spaces via MMIO register operations.
pub(crate) struct AddressSpaceManager<'bound> {
    /// Memory-mapped I/O region for GPU register access.
    iomem: Arc<IoMem<'bound>>,
}

impl<'bound> AddressSpaceManager<'bound> {
    /// Creates a new address space manager.
    ///
    /// Initializes the manager with references to the platform device and
    /// I/O memory region, along with the bitmask of available AS slots.
    pub(super) fn new(iomem: ArcBorrow<'_, IoMem<'bound>>) -> Result<AddressSpaceManager<'bound>> {
        Ok(Self {
            iomem: iomem.into(),
        })
    }

    pub(super) fn activate_vm(&mut self) -> Result {
        self.as_wait_ready(0)
    }

    /// Waits for an AS slot to become ready (not active).
    ///
    /// Returns an error if polling times out after 10ms or if register access fails.
    fn as_wait_ready(&self, as_nr: usize) -> Result {
        let io = &*self.iomem;
        let op = || {
            let status_reg = STATUS::try_at(as_nr).ok_or(EINVAL)?;
            Ok(io.read(status_reg))
        };
        let cond = |status: &STATUS| -> bool { !status.active_ext() };
        poll::read_poll_timeout(op, cond, Delta::from_millis(0), Delta::from_millis(10))?;

        Ok(())
    }
}
