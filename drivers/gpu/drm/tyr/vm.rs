// SPDX-License-Identifier: GPL-2.0 or MIT

//! GPU virtual memory management.
//!
//! This is just a lifetime/resource stub. The actual vm
//! implementation using the DRM GPUVM framework will be added later.

use kernel::{
    device::Bound,
    drm::DeviceContext,
    platform,
    prelude::*,
    sync::{
        aref::ARef,
        Arc,
        ArcBorrow, //
    }, //
};

use crate::{
    driver::TyrDrmDevice,
    gpu::GpuInfo,
    mmu::Mmu, //
};

/// GPU virtual address space.
///
/// Each VM can be mapped into a hardware address space slot.
#[pin_data]
pub(crate) struct Vm<'bound> {
    /// MMU manager.
    mmu: Arc<Mmu<'bound>>,
    /// Platform device reference (needed to access the page table via devres).
    pdev: ARef<platform::Device>,
}

impl<'bound> Vm<'bound> {
    /// Creates a new GPU virtual address space.
    pub(crate) fn new<Ctx: DeviceContext>(
        pdev: &'bound platform::Device<Bound>,
        _ddev: &TyrDrmDevice<Ctx>,
        mmu: ArcBorrow<'_, Mmu<'bound>>,
        _gpu_info: &GpuInfo,
    ) -> Result<Arc<Vm<'bound>>> {
        let vm = Arc::pin_init(
            pin_init!(Self {
                pdev: pdev.into(),
                mmu: mmu.into(),
            }),
            GFP_KERNEL,
        )?;

        Ok(vm)
    }

    /// Activate the VM in a hardware address space slot.
    pub(crate) fn activate(&self) -> Result {
        self.mmu.activate_vm().inspect_err(|e| {
            pr_err!("Failed to activate VM: {:?}\n", e);
        })
    }
}
