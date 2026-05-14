// SPDX-License-Identifier: GPL-2.0 or MIT

use kernel::{
    bindings,
    clk::{
        Clk,
        OptionalClk, //
    },
    device::{
        Bound,
        Core,
        Device, //
    },
    devres::Devres,
    drm,
    drm::ioctl,
    io::poll,
    new_mutex,
    of,
    platform,
    pm::*,
    prelude::*,
    regulator,
    regulator::Regulator,
    sizes::SZ_2M,
    sync::{
        aref::ARef,
        Arc,
        Mutex, //
    },
    time, //
};

use crate::{
    file::TyrDrmFileData,
    gem::TyrObject,
    gpu,
    gpu::GpuInfo,
    regs, //
};

pub(crate) type IoMem = kernel::io::mem::IoMem<SZ_2M>;

pub(crate) struct TyrDrmDriver;

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type TyrDrmDevice = drm::Device<TyrDrmDriver>;

#[pin_data(PinnedDrop)]
pub(crate) struct TyrPlatformDriverData {
    _device: ARef<TyrDrmDevice>,
}

#[pin_data(PinnedDrop)]
pub(crate) struct TyrDrmDeviceData {
    pub(crate) pdev: ARef<platform::Device>,

    #[pin]
    regulators: Mutex<Regulators>,

    /// Runtime PM context
    pub(crate) pm_context: PMContext<TyrPlatformDriverData, kernel::pm::Active>,

    /// Some information on the GPU.
    ///
    /// This is mainly queried by userspace, i.e.: Mesa.
    pub(crate) gpu_info: GpuInfo,
}

fn issue_soft_reset(dev: &Device<Bound>, iomem: &Devres<IoMem>) -> Result {
    regs::GPU_CMD.write(dev, iomem, regs::GPU_CMD_SOFT_RESET)?;

    poll::read_poll_timeout(
        || regs::GPU_IRQ_RAWSTAT.read(dev, iomem),
        |status| *status & regs::GPU_IRQ_RAWSTAT_RESET_COMPLETED != 0,
        time::Delta::from_millis(1),
        time::Delta::from_millis(100),
    )
    .inspect_err(|_| dev_err!(dev, "GPU reset failed."))?;

    Ok(())
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <TyrPlatformDriverData as platform::Driver>::IdInfo,
    [
        (of::DeviceId::new(c"rockchip,rk3588-mali"), ()),
        (of::DeviceId::new(c"arm,mali-valhall-csf"), ())
    ]
);

impl platform::Driver for TyrPlatformDriverData {
    type IdInfo = ();
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const PM_OPS: Option<&'static bindings::dev_pm_ops> = Some(&PMContext::<Self>::PM_OPS);

    fn probe(
        pdev: &platform::Device<Core>,
        _info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        let pm_state = TyrRuntimeState::pm_init(pdev.as_ref())?;
        let mali_regulator = Regulator::<regulator::Enabled>::get(pdev.as_ref(), c"mali")?;
        let sram_regulator = Regulator::<regulator::Enabled>::get(pdev.as_ref(), c"sram")?;

        let request = pdev.io_request_by_index(0).ok_or(ENODEV)?;
        let iomem = Arc::pin_init(request.iomap_sized::<SZ_2M>(), GFP_KERNEL)?;

        let pm_context = PMContext::new(pdev.as_ref(), None)?;
        pm_context.apply_config(&[PMConfig::AutoSuspend(true),PMConfig::AutoSuspendDelay(300)])?;
        pm_context.init_state(RuntimePMState::Active, Some(pm_state))?;

        issue_soft_reset(pdev.as_ref(), &iomem)?;
        gpu::l2_power_on(pdev.as_ref(), &iomem)?;

        let gpu_info = GpuInfo::new(pdev.as_ref(), &iomem)?;
        gpu_info.log(pdev);

        let platform: ARef<platform::Device> = pdev.into();

        let data = try_pin_init!(TyrDrmDeviceData {
                pdev: platform.clone(),
                regulators <- new_mutex!(Regulators {
                    _mali: mali_regulator,
                    _sram: sram_regulator,
                }),
                pm_context: pm_context.enable()?,
                gpu_info,
        });

        let ddev: ARef<TyrDrmDevice> = drm::Device::new(pdev.as_ref(), data)?;
        drm::driver::Registration::new_foreign_owned(&ddev, pdev.as_ref(), 0)?;

        let driver = TyrPlatformDriverData { _device: ddev };

        // We need this to be dev_info!() because dev_dbg!() does not work at
        // all in Rust for now, and we need to see whether probe succeeded.
        dev_info!(pdev, "Tyr initialized correctly.\n");
        Ok(driver)
    }
}

#[pinned_drop]
impl PinnedDrop for TyrPlatformDriverData {
    fn drop(self: Pin<&mut Self>) {}
}

#[pinned_drop]
impl PinnedDrop for TyrDrmDeviceData {
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
    type Data = TyrDrmDeviceData;
    type File = TyrDrmFileData;
    type Object = drm::gem::Object<TyrObject>;

    const INFO: drm::DriverInfo = INFO;

    kernel::declare_drm_ioctls! {
        (PANTHOR_DEV_QUERY, drm_panthor_dev_query, ioctl::RENDER_ALLOW, TyrDrmFileData::dev_query),
    }
}

#[pin_data]
struct Clocks {
    core: Clk,
    stacks: OptionalClk,
    coregroup: OptionalClk,
}

#[pin_data]
struct Regulators {
    _mali: Regulator<regulator::Enabled>,
    _sram: Regulator<regulator::Enabled>,
}

pub(crate) struct TyrRuntimeState {
    clks: Clocks,
}

impl TyrRuntimeState {
    fn pm_init(dev: &'_ kernel::device::Device<kernel::device::Bound>) -> Result<Arc<Self>> {
         let core_clk = Clk::get(dev, Some(c"core"))?;
         let stacks_clk = OptionalClk::get(dev, Some(c"stacks"))?;
         let coregroup_clk = OptionalClk::get(dev, Some(c"coregroup"))?;

         core_clk.prepare_enable()?;
         stacks_clk.prepare_enable()?;
         coregroup_clk.prepare_enable()?;

         Ok(Arc::new(
             Self {
                clks: Clocks {
                     core: core_clk,
                    stacks: stacks_clk,
                    coregroup: coregroup_clk,
                 }
            },
            GFP_KERNEL
        )?)
    }
}

#[vtable]
impl PMOps for TyrPlatformDriverData {
    type DriverDataType = TyrPlatformDriverData;
    type DeviceType<'a> = &'a platform::Device;
    type RuntimePayloadType = TyrRuntimeState;
    type PMContextRef<'a> = &'a PMContext<TyrPlatformDriverData, Active>;

    fn get_pmcontext(data: &Self::DriverDataType) -> Result<Self::PMContextRef<'_>> {
        Ok(&data._device.pm_context)
    }

    fn runtime_suspend(
        _dev:  Self::DeviceType<'_>,
        data: Option<Arc<Self::RuntimePayloadType>>
    ) -> Result<
            Option<Arc<Self::RuntimePayloadType>>,
            (Option<Arc<Self::RuntimePayloadType>>, kernel::error::Error)
    > {
        match data {
            Some(state) => {
                state.clks.core.disable_unprepare();
                state.clks.stacks.disable_unprepare();
                state.clks.coregroup.disable_unprepare();

                Ok(Some(state))
            }
            None => {
                pr_err!("Tyr: no runtime data\n");
                Ok(None)
            }
        }
    }

    fn runtime_resume(
        _dev: Self::DeviceType<'_>,
        data: Option<Arc<Self::RuntimePayloadType>>
    ) -> Result<
            Option<Arc<Self::RuntimePayloadType>>,
            (Option<Arc<Self::RuntimePayloadType>>, kernel::error::Error)
    > {

        match data {
            Some(state) => {
                let previous_state = Some(state.clone());
                (|| {
                    //@TODO: @FIXME: this needs to be able to unroll changes
                    state.clks.core.prepare_enable()?;
                    state.clks.stacks.prepare_enable()?;
                    state.clks.coregroup.prepare_enable()?;

                    Ok(Some(state))
                }
                )()
                .map_err(|e| (previous_state, e))
            },
            None => Err((data, EINVAL)),
        }
    }
}
