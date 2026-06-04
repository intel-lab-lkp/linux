// SPDX-License-Identifier: GPL-2.0 or MIT

//! Memory Management Unit (MMU) driver for the Tyr GPU.
//!
//! This is just a lifetime/resource stub.
//! The actual MMU initialization, page table management,
//! and GPU VM mapping will be added later.

use kernel::{
    new_mutex,
    prelude::*,
    sync::{
        Arc,
        ArcBorrow,
        Mutex, //
    }, //
};

use crate::{
    driver::IoMem,
    gpu::GpuInfo,
    mmu::address_space::AddressSpaceManager,
    regs::gpu_control::AS_PRESENT, //
};

pub(crate) mod address_space;

#[pin_data]
pub(super) struct Mmu<'bound> {
    /// Manages the allocation of hardware MMU slots to GPU address spaces.
    #[pin]
    pub(crate) as_manager: Mutex<AddressSpaceManager<'bound>>,
}

impl<'bound> Mmu<'bound> {
    /// Create an MMU component for this device.
    pub(super) fn new(
        iomem: ArcBorrow<'_, IoMem<'bound>>,
        gpu_info: &GpuInfo,
    ) -> Result<Arc<Mmu<'bound>>> {
        let present = AS_PRESENT::from_raw(gpu_info.as_present).present().get();
        let slot_count: usize = present.count_ones().try_into()?;
        pr_info!("MMU: {} address space slots present", slot_count);

        let as_manager = AddressSpaceManager::new(iomem)?;
        let mmu_init = try_pin_init!(Self{
            as_manager <- new_mutex!(as_manager),
        });
        Arc::pin_init(mmu_init, GFP_KERNEL)
    }

    /// Make a VM active.
    pub(crate) fn activate_vm(&self) -> Result {
        self.as_manager.lock().activate_vm()
    }
}
