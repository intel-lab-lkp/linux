// SPDX-License-Identifier: GPL-2.0 or MIT

//! Firmware loading and management for Mali CSF GPUs.
//!
//! This is currently only a lifetime/resource stub. The actual firmware parser,
//! section loading, MCU VM mapping, and boot sequence will be added later.
#![allow(dead_code)]

use kernel::{
    device::Bound,
    drm::Uninit,
    io::{
        poll,
        Io, //
    },
    platform,
    prelude::*,
    sync::{
        Arc,
        ArcBorrow, //
    },
    time::Delta, //
};

use crate::{
    driver::{
        IoMem,
        TyrDrmDevice, //
    },
    gpu::GpuInfo,
    mmu::Mmu,
    regs::gpu_control::{
        McuControlMode,
        McuStatus,
        MCU_CONTROL,
        MCU_STATUS, //
    },
    vm::Vm, //
};

/// Firmware state for a bound Tyr device.
///
/// For now this only keeps the device resources alive. Later this will own the
/// loaded firmware sections, MCU VM mappings, and boot state.
pub(crate) struct Firmware<'bound> {
    /// Parent platform device.
    _pdev: &'bound platform::Device<Bound>,

    /// MMIO mapping used for firmware/MCU register access.
    iomem: Arc<IoMem<'bound>>,
}

impl<'bound> Firmware<'bound> {
    /// Create firmware state for this device.
    ///
    /// This stub only records the resources that future firmware loading code
    /// will need.
    pub(crate) fn new(
        pdev: &'bound platform::Device<Bound>,
        iomem: Arc<IoMem<'bound>>,
        ddev: &TyrDrmDevice<Uninit>,
        mmu: ArcBorrow<'_, Mmu<'bound>>,
        gpu_info: &GpuInfo,
    ) -> Result<Firmware<'bound>> {
        let vm = Vm::new(pdev, ddev, mmu, gpu_info)?;
        vm.activate()?;

        Ok(Firmware { _pdev: pdev, iomem })
    }

    /// Boot the firmware.
    pub(crate) fn boot(&self) -> Result {
        let io = &self.iomem;
        io.write_reg(MCU_CONTROL::zeroed().with_req(McuControlMode::Auto));

        if let Err(e) = poll::read_poll_timeout(
            || Ok(io.read(MCU_STATUS)),
            |status| status.value() == McuStatus::Enabled,
            Delta::from_millis(1),
            Delta::from_millis(100),
        ) {
            let status = io.read(MCU_STATUS);
            pr_err!("MCU failed to boot, status: {:?}", status.value());
            return Err(e);
        }
        Ok(())
    }
}
