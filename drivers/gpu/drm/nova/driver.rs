// SPDX-License-Identifier: GPL-2.0

use alloc::boxed::Box;
use core::pin::Pin;
use kernel::{
    bindings, c_str, device, driver, drm,
    drm::{drv, ioctl},
    pci,
    pci::define_pci_id_table,
    prelude::*,
    sync::Arc,
};

use crate::{file::File, gpu::Gpu};

pub(crate) struct NovaDriver;

/// Convienence type alias for the DRM device type for this driver
pub(crate) type NovaDevice = drm::device::Device<NovaDriver>;

#[allow(dead_code)]
pub(crate) struct NovaData {
    pub(crate) gpu: Arc<Gpu>,
    pub(crate) pdev: pci::Device,
}

type DeviceData = device::Data<drm::drv::Registration<NovaDriver>, NovaData>;

const INFO: drm::drv::DriverInfo = drm::drv::DriverInfo {
    major: 0,
    minor: 0,
    patchlevel: 0,
    name: c_str!("nova"),
    desc: c_str!("Nvidia Graphics"),
    date: c_str!("20240227"),
};

impl pci::Driver for NovaDriver {
    type Data = Arc<DeviceData>;

    define_pci_id_table! {
        (),
        [ (pci::DeviceId::new(bindings::PCI_VENDOR_ID_NVIDIA, bindings::PCI_ANY_ID as u32), None) ]
    }

    fn probe(pdev: &mut pci::Device, _id_info: Option<&Self::IdInfo>) -> Result<Arc<DeviceData>> {
        dev_dbg!(pdev.as_ref(), "Probe Nova GPU driver.\n");

        let reg = drm::drv::Registration::<NovaDriver>::new(pdev.as_ref())?;

        pdev.enable_device_mem()?;
        pdev.set_master();

        let bar = pdev.iomap_region(0, c_str!("nova"))?;

        let gpu = Gpu::new(pdev, bar)?;

        let data = kernel::new_device_data!(
            reg,
            NovaData {
                gpu,
                pdev: pdev.clone(),
            },
            "NovaData"
        )?;
        let data: Arc<DeviceData> = data.into();

        kernel::drm_device_register!(
            data.registrations().ok_or(ENXIO)?.as_pinned_mut(),
            data.clone(),
            0
        )?;

        Ok(data)
    }

    fn remove(data: &Self::Data) {
        dev_dbg!(data.pdev.as_ref(), "Remove Nova GPU driver.\n");
    }
}

#[vtable]
impl drm::drv::Driver for NovaDriver {
    type Data = Arc<DeviceData>;
    type File = File;
    type Object = crate::gem::Object;

    const INFO: drm::drv::DriverInfo = INFO;
    const FEATURES: u32 = drv::FEAT_GEM;

    kernel::declare_drm_ioctls! {
        (NOVA_GETPARAM, drm_nova_getparam, ioctl::RENDER_ALLOW, File::get_param),
        (NOVA_GEM_CREATE, drm_nova_gem_create, ioctl::AUTH | ioctl::RENDER_ALLOW, File::gem_create),
        (NOVA_GEM_INFO, drm_nova_gem_info, ioctl::AUTH | ioctl::RENDER_ALLOW, File::gem_info),
    }
}

pub(crate) struct NovaModule {
    _registration: Pin<Box<driver::Registration<pci::Adapter<NovaDriver>>>>,
}

impl kernel::Module for NovaModule {
    fn init(_name: &'static CStr, module: &'static ThisModule) -> Result<Self> {
        let registration = driver::Registration::new_pinned(c_str!("nova"), module)?;

        Ok(Self {
            _registration: registration,
        })
    }
}
