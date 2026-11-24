// SPDX-License-Identifier: GPL-2.0 or MIT

use kernel::{
    clk::{
        Clk,
        OptionalClk, //
    },
    device::{
        Bound,
        Core,
        Device,
        DeviceContext,
        pm:: {
            self,
            *,
        }, //
    },
    dma::{
        Device as DmaDevice,
        DmaMask, //
    },
    drm,
    drm::ioctl,
    io::{
        poll,
        Io, //
    },
    of,
    platform,
    prelude::*,
    regulator,
    regulator::Regulator,
    sizes::SZ_2M,
    sync::Arc,
    time, //
};

use crate::{
    file::TyrDrmFileData,
    fw::Firmware,
    gem::Bo,
    gpu,
    gpu::GpuInfo,
    mmu::Mmu,
    regs::gpu_control::*, //
};

pub(crate) type IoMem<'a> = kernel::io::mem::IoMem<'a, SZ_2M>;
pub(crate) type TyrRegisters = kernel::io::Region<SZ_2M>;

pub(crate) struct TyrDrmDriver;

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type TyrDrmDevice<Ctx = drm::Normal> = drm::Device<TyrDrmDriver, Ctx>;

pub(crate) struct TyrPlatformDriver;

#[pin_data]
pub(crate) struct TyrHW<'bound> {
    /// Firmware sections.
    pub(crate) fw: Firmware<'bound>,

    /// GPU MMIO register mapping.
    pub(crate) iomem: Arc<IoMem<'bound>>,

     /// GPU information read from hardware during probe.
    pub(crate) gpu_info: GpuInfo,
}


#[pin_data(PinnedDrop)]
pub(crate) struct TyrPlatformDriverData<'bound> {
    #[pin]
    #[uses('pm: invariant, 'hw)]
    _reg: drm::Registration<'pm, TyrDrmDriver>,
    // This needs to be dropped after drm::Registration as that one
    // borrows PMContext.
    #[pin]
    #[uses('hw: invariant)]
    pub(crate) pm: pm::Registration<'hw, TyrPlatformDriver>,
    #[pin]
    hw: TyrHW<'bound>,
}

#[pin_data]
pub(crate) struct TyrPMRegistrationData<'pm>
where
    exists<'bound>: 'pm,
{
    #[uses('bound: invariant)]
    pub(crate) hw: &'pm TyrHW<'bound>,
}


/// Data owned by the DRM [`Registration`].
///
/// This data can have references tied to the parent platform device binding scope
/// and is accessible only while the DRM device is registered with userspace.
#[pin_data]
pub(crate) struct TyrDrmRegistrationData<'drm>
where
    exists<'bound>: 'drm,
{
    /// Parent platform device.
    pub(crate) pdev: &'drm platform::Device<Bound>,

    /// Runtime PM context owned by the PM Registration
    pub(crate) pm: PMContext<'drm, 'drm, TyrPlatformDriver>,
    #[uses('bound: invariant)]
    pub(crate) hw: &'drm TyrHW<'bound>,
}

fn issue_soft_reset(dev: &Device, iomem: &IoMem<'_>) -> Result {
    iomem.write_reg(GPU_COMMAND::reset(ResetMode::SoftReset));

    poll::read_poll_timeout(
        || Ok(iomem.read(GPU_IRQ_RAWSTAT)),
        |status| status.reset_completed(),
        time::Delta::from_millis(1),
        time::Delta::from_millis(100),
    )
    .inspect_err(|_| dev_err!(dev, "GPU reset failed."))?;

    Ok(())
}

kernel::of_device_table!(
    OF_TABLE,
    <TyrPlatformDriver as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c"rockchip,rk3588-mali"), ()),
        (of::DeviceId::new(c"arm,mali-valhall-csf"), ())
    ]
);

impl platform::Driver for TyrPlatformDriver {
    type IdInfo = ();
    type Data<'bound> = TyrPlatformDriverData<'bound>;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const DEV_PM_OPS: Option<pm::OpsRef<Self>> = Some(TYR_PM_OPS.pm_ops());

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        pin_init::pin_init_scope(move || {
            let core_clk = Clk::get(pdev.as_ref(), Some(c"core"))?;
            let stacks_clk = OptionalClk::get(pdev.as_ref(), Some(c"stacks"))?;
            let coregroup_clk = OptionalClk::get(pdev.as_ref(), Some(c"coregroup"))?;

            core_clk.prepare_enable()?;
            stacks_clk.prepare_enable()?;
            coregroup_clk.prepare_enable()?;

            let mali_regulator = Regulator::<regulator::Enabled>::get(pdev.as_ref(), c"mali")?;
            let sram_regulator = Regulator::<regulator::Enabled>::get(pdev.as_ref(), c"sram")?;

            let runtime_payload = TyrRuntimePMPayload {
                clks: Clocks {
                    core: core_clk,
                    stacks: stacks_clk,
                    coregroup: coregroup_clk,
                },
                _regulators: Regulators {
                    _mali: mali_regulator,
                    _sram: sram_regulator,
                },
            };

            let mut pm_configs = KVec::<PMConfig>::with_capacity(2, GFP_KERNEL)?;
            pm_configs.push(PMConfig::AutoSuspend(true), GFP_KERNEL)?;
            pm_configs.push(PMConfig::AutoSuspendDelay(300), GFP_KERNEL)?;

            let request = pdev.io_request_by_index(0).ok_or(ENODEV)?;

            let iomem = Arc::new(request.iomap_sized::<SZ_2M>()?, GFP_KERNEL)?;

            issue_soft_reset(pdev.as_ref(), &iomem)?;
            gpu::l2_power_on(pdev.as_ref(), &iomem)?;

            let gpu_info = GpuInfo::new(&iomem);
            gpu_info.log(pdev.as_ref());

            let pa_bits = MMU_FEATURES::from_raw(gpu_info.mmu_features)
                .pa_bits()
                .get();
            // SAFETY: No concurrent DMA allocations or mappings can be made because
            // the device is still being probed and therefore isn't being used by
            // other threads of execution.
            unsafe { pdev.dma_set_mask_and_coherent(DmaMask::try_new(pa_bits)?)? };

            let unreg_dev = drm::UnregisteredDevice::<TyrDrmDriver>::new(pdev, Ok(()))?;

            Ok(try_pin_init!(TyrPlatformDriverData {
                    hw <- {
                        let mmu = Mmu::new(pdev.as_ref(), iomem.as_arc_borrow(), &gpu_info)?;

                        let firmware = Firmware::new(
                            pdev.as_ref(),
                            iomem.clone(),
                            &unreg_dev,
                            mmu.as_arc_borrow(),
                            &gpu_info,
                        )?;

                        firmware.boot()?;

                        pin_init!(TyrHW {
                            fw: firmware,
                            iomem,
                            gpu_info
                        })
                },
                pm <- {
                    // SAFETY: `pm` is owned by the driver data and remains accounted for
                    unsafe {
                        pm::Registration::new(
                            pdev.as_ref(),
                            &TYR_PM_OPS,
                            None,
                            Some(pm_configs),
                            pin_init!(TyrPMRegistrationData{hw: hw.get_ref()}),
                            pm::RuntimeStatePayload::Active(runtime_payload)
                        )
                    }
                },

                _reg <- {
                    // SAFETY: `_reg` is owned by the driver data and is not forgotten.
                    unsafe {
                        drm::Registration::new(
                            pdev.as_ref(),
                            unreg_dev,
                            pin_init!(TyrDrmRegistrationData {
                                pdev,
                                pm: pm.get_ref().get_ctx(),
                                hw: hw.get_ref(),
                            }),
                        )
                    }
                },

                _: {
                    pm.get_ctx().enable(RuntimePMState::Active)?;
                    dev_dbg!(pdev, "Tyr initialized correctly.");
                },
            }))
        })
    }
}

#[pinned_drop]
impl PinnedDrop for TyrPlatformDriverData<'_> {
    fn drop(self: Pin<&mut Self>) {}
}

// We need to retain the name "panthor" to achieve drop-in compatibility with
// the C driver in the userspace stack.
const INFO: drm::DriverInfo = drm::DriverInfo {
    major: 1,
    minor: 5,
    patchlevel: 0,
    name: c"panthor",
    desc: c"ARM Mali Tyr DRM driver",
};

#[vtable]
impl drm::Driver for TyrDrmDriver {
    type Data = ();
    type RegistrationData<'drm> = TyrDrmRegistrationData<'drm>;
    type File = TyrDrmFileData;
    type Object = Bo;
    type ParentDevice<Ctx: DeviceContext> = platform::Device<Ctx>;

    const INFO: drm::DriverInfo = INFO;
    const FEAT_RENDER: bool = true;

    kernel::declare_drm_ioctls! {
        (PANTHOR_DEV_QUERY, drm_panthor_dev_query, ioctl::RENDER_ALLOW, TyrDrmFileData::dev_query),
    }
}

struct Clocks {
    core: Clk,
    stacks: OptionalClk,
    coregroup: OptionalClk,
}

impl Drop for Clocks {
    fn drop(&mut self) {
        self.core.disable_unprepare();
        self.stacks.disable_unprepare();
        self.coregroup.disable_unprepare();
    }
}

struct Regulators {
    _mali: Regulator<regulator::Enabled>,
    _sram: Regulator<regulator::Enabled>,
}

pub(crate) struct TyrRuntimePMPayload {
    clks: Clocks,
    _regulators: Regulators,
}

static TYR_PM_OPS: pm::Ops<TyrPlatformDriver> = pm::Ops::new();

#[vtable]
impl PMOps for TyrPlatformDriver {
    type Adapter = platform::Adapter<TyrPlatformDriver>;
    type DeviceType = platform::Device<kernel::device::Bound>;
    type RegistrationData<'a> = TyrPMRegistrationData<'a>;
    type ActivePayload<'a> = TyrRuntimePMPayload;
    type SuspendedPayload<'a> = TyrRuntimePMPayload;

    fn runtime_suspend<'a>(
        _dev: &'a Self::DeviceType,
        _reg_data: &'a Self::RegistrationData<'a>,
        payload: Self::ActivePayload<'a>,
    ) -> Result<Self::SuspendedPayload<'a>, (Self::ActivePayload<'a>, Error)> {
        payload.clks.coregroup.disable_unprepare();
        payload.clks.stacks.disable_unprepare();
        payload.clks.core.disable_unprepare();
        Ok(payload)
    }
    fn runtime_resume<'a>(
        _dev: &'a Self::DeviceType,
        _reg_data: &'a Self::RegistrationData<'a>,
        payload: Self::SuspendedPayload<'a>,
    ) -> Result<Self::ActivePayload<'a>, (Self::SuspendedPayload<'a>, Error)> {
        if let Err(e) = payload.clks.core.prepare_enable() {
            return Err((payload, e));
        }

        if let Err(e) = payload.clks.stacks.prepare_enable() {
            payload.clks.core.disable_unprepare();
            return Err((payload, e));
        }

        if let Err(e) = payload.clks.coregroup.prepare_enable() {
            payload.clks.stacks.disable_unprepare();
            payload.clks.core.disable_unprepare();
            return Err((payload, e));
        }

        Ok(payload)
    }
}
